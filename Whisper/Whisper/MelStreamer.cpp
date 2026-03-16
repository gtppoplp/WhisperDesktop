#include "stdafx.h"
#include "MelStreamer.h"
#include "../Utils/parallelFor.h"
using namespace Whisper;

using PcmQueueLock = CComCritSecLock<CComAutoCriticalSection>;

MelStreamer::MelStreamer( const Filters& filters, ProfileCollection& prof, const iAudioReader* iar ) :
	reader( iar ),
	melContext( filters ),
    melCount( filters.n_mel ),
	profiler( prof )
{ }

void MelStreamer::dropOldChunks( size_t off )
{
	const bool stereo = reader.outputsStereo();
	PcmQueueLock pcmGuard( pcmLock );
	for( size_t i = streamStartOffset; i < off; i++ )
	{
		queuePcmMono.pop_front();
		queueMel.pop_front();
		if( stereo )
			queuePcmStereo.pop_front();
	}
	streamStartOffset = off;
}

HRESULT MelStreamer::ensurePcmChunks( size_t len )
{
	const bool loadStereo = reader.outputsStereo();

	const size_t neededChunks = len + FFT_SIZE / FFT_STEP;
	while( true )
	{
		{
			PcmQueueLock lock( pcmLock );
			if( readerEof )
				return queuePcmMono.empty() ? E_EOF : S_FALSE;
			if( queuePcmMono.size() >= neededChunks )
				return S_OK;
		}

		PcmMonoChunk mono;
		PcmStereoChunk stereo;
		PcmStereoChunk* stereoPtr = loadStereo ? &stereo : nullptr;
		HRESULT hr = reader.readChunk( mono, stereoPtr );
		if( SUCCEEDED( hr ) )
		{
			PcmQueueLock lock( pcmLock );
			queuePcmMono.push_back( mono );
			if( loadStereo )
				queuePcmStereo.push_back( stereo );
			continue;
		}

		if( hr == E_EOF )
		{
			PcmQueueLock lock( pcmLock );
			readerEof = true;
			return queuePcmMono.empty() ? E_EOF : S_FALSE;
		}

		return hr;
	}
}

size_t MelStreamer::serializePcm( size_t startChunkAbsolute )
{
	PcmQueueLock lock( pcmLock );
	if( startChunkAbsolute < streamStartOffset )
		return 0;
	const size_t relativeOffset = startChunkAbsolute - streamStartOffset;
	if( relativeOffset >= queuePcmMono.size() )
		return 0; // Caller requested data that has been trimmed or not yet produced
	const size_t chunks = queuePcmMono.size() - relativeOffset;
	if( chunks == 0 )
		return 0;

	tempPcm.resize( chunks * FFT_STEP );
	float* rdi = tempPcm.data();

	for( auto it = queuePcmMono.begin() + relativeOffset; it != queuePcmMono.end(); it++ )
	{
		memcpy( rdi, it->mono.data(), FFT_STEP * 4 );
		rdi += FFT_STEP;
	}
	return chunks;
}

void MelStreamer::makeTransposedBuffer( size_t off, size_t len )
{
	// Resize the output
	assert( len <= queueMel.size() );
   outputMel.resize( len * melCount );

    // First pass, copy transposed MEL data, and compute the maximum
	float mmax = 1e-20f;
	for( size_t i = 0; i < len; i++ )
	{
		const float* const src = queueMel[ i ].data();
		for( size_t j = 0; j < melCount; j++ )
		{
			const float v = src[ j ];
			outputMel[ j * len + i ] = v;
			mmax = std::max( mmax, v );
		}
	}

	// Second pass, clamping and normalization
	const size_t bufferEnd = off + len;
	if( lastBufferEnd != bufferEnd )
	{
		// Store maximum value in this class, along with the end sample index
		lastBufferEnd = bufferEnd;
		lastBufferMax = mmax;
	}
	else
	{
		// We're probably at the and of the stream, the caller asked for a smalled slice of the samples with the same end as the last time.
		// Discard the computed maximum value, and instead use the number stored in this class
		mmax = lastBufferMax;
	}

   mmax -= 8.0f;
	for( float& v : outputMel )
	{
		if( v < mmax )
			v = mmax;
		v = ( v + 4.0f ) * 0.25f;
	}
}

HRESULT MelStreamerSimple::makeBuffer( size_t off, size_t len, const float** buffer, size_t& stride ) noexcept
{
	if( off < streamStartOffset )
	{
		logError( u8"MelStreamer doesn't support backwards seeks" );
		return E_UNEXPECTED;
	}

	if( off > streamStartOffset )
	{
		// The model wants to advance forward, drop now irrelevant chunks of data
		dropOldChunks( off );
	}

	// Compute all these MEL chunks
	const size_t availableMel = queueMel.size();
	if( availableMel < len )
	{
		CHECK( ensurePcmChunks( len ) );

		const size_t pcmChunks = serializePcm( streamStartOffset + availableMel );
		const size_t missingMelChunks = len - availableMel;
		size_t i;
		const size_t loop1 = std::min( missingMelChunks, pcmChunks );
		{
			auto profilerBlock = profiler.cpuBlock( eCpuBlock::Spectrogram );
			for( i = 0; i < loop1; i++ )
			{
				// if( readerEof && i + 1 == loop1 ) __debugbreak();
				auto& arr = queueMel.emplace_back();
				const float* sourcePcm = tempPcm.data() + i * FFT_STEP;
				size_t availableChunks = pcmChunks - i;
				size_t availableFloats = availableChunks * FFT_STEP;
				melContext.fft( arr, sourcePcm, availableFloats );
			}
		}
		for( ; i < missingMelChunks; i++ )
		{
			assert( readerEof );
			auto& arr = queueMel.emplace_back();
         memset( arr.data(), 0, melCount * 4 );
		}
	}

	// Produce the result
	makeTransposedBuffer( off, len );
	stride = len;
	*buffer = outputMel.data();
	return S_OK;
}

MelStreamerThread::MelStreamerThread( const Filters& filters, ProfileCollection& profiler, const iAudioReader* iar, int countThreads ) :
	MelStreamer( filters, profiler, iar ),
	workerThreads( countThreads )
{
	if( workerThreads > 1 )
	{
		check( ThreadPoolWork::create() );
		melContextsWorkers.reserve( workerThreads - 1 );
		for( int i = 1; i < workerThreads; i++ )
			melContextsWorkers.emplace_back( filters );
	}

	InitializeConditionVariable( &wakeMain );
	InitializeConditionVariable( &wakeBackground );
	threadStatus = eThreadStatus::NotStarted;
	const HANDLE h = CreateThread( nullptr, 0, &threadProcStatic, this, 0, nullptr );
	if( nullptr == h )
		throw HRESULT_FROM_WIN32( GetLastError() );
	threadHandle.Attach( h );
}

using Lock = CComCritSecLock<CComAutoCriticalSection>;

constexpr ptrdiff_t prebufferChunks = 3000 * 2;
constexpr ptrdiff_t chunksPerWakeup = 512;
constexpr ptrdiff_t minChunksPerThread = 64;

HRESULT MelStreamerThread::threadMain()
{
	pendingChunks.reserve( chunksPerWakeup );

	EnterCriticalSection( &m_cs.m_sec );
	threadStatus = eThreadStatus::Working;

	while( true )
	{
		if( shuttingDown )
		{
			LeaveCriticalSection( &m_cs.m_sec );
			return S_FALSE;
		}

		// Count of available MEL chunks
		const ptrdiff_t availableMel = queueMel.size();
		if( availableMel >= prebufferChunks )
		{
			threadStatus = eThreadStatus::Idle;
			SleepConditionVariableCS( &wakeBackground, &m_cs.m_sec, INFINITE );
			threadStatus = eThreadStatus::Working;
			continue;
		}
		// Count of MEL chunks remaining in the whole stream
		// availableMel of them are already on the queue
		const ptrdiff_t remainingMel = (ptrdiff_t)getLength() - (ptrdiff_t)streamStartOffset;
		const size_t startAbsolute = streamStartOffset + (size_t)availableMel;
		LeaveCriticalSection( &m_cs.m_sec );

		const ptrdiff_t missingChunks = prebufferChunks - availableMel;
		ptrdiff_t chunks = std::min( missingChunks, chunksPerWakeup );
		chunks = std::min( chunks, remainingMel - availableMel );
		if( chunks <= 0 )
			return S_OK; // This thread has produced all chunks of the stream

		CHECK( ensurePcmChunks( availableMel + chunks ) );
		const size_t pcmChunks = serializePcm( startAbsolute );
		if( 0 == pcmChunks )
		{
			EnterCriticalSection( &m_cs.m_sec );
			continue;
		}

		pendingChunks.clear();

		chunks = std::min( chunks, (ptrdiff_t)pcmChunks );
		{
			auto profilerBlock = profiler.cpuBlock( eCpuBlock::Spectrogram );

			if( this->workerThreads <= 1 || chunks < minChunksPerThread * 2 )
			{
				// Thread pool disabled with a setting, or not enough work for the thread pool
				for( ptrdiff_t i = 0; i < chunks; i++ )
				{
					MelChunk& arr = pendingChunks.emplace_back();
					const float* sourcePcm = tempPcm.data() + i * FFT_STEP;
					size_t availableChunks = pcmChunks - i;
					size_t availableFloats = availableChunks * FFT_STEP;
					melContext.fft( arr, sourcePcm, availableFloats );
				}
			}
			else
			{
				// Use thread pool for these FFTs
				pendingChunks.resize( chunks );
				int nth = (int)( ( chunks + minChunksPerThread - 1 ) / minChunksPerThread );
				nth = std::min( nth, this->workerThreads );
				assert( nth > 1 );
				this->fftChunks = (int)chunks;
				this->fftThreads = nth;
				CHECK( ThreadPoolWork::parallelFor( nth ) );
			}
		}

		EnterCriticalSection( &m_cs.m_sec );
		if( shuttingDown )
		{
			LeaveCriticalSection( &m_cs.m_sec );
			return S_FALSE;
		}

		for( const auto& a : pendingChunks )
			queueMel.push_back( a );

		LeaveCriticalSection( &m_cs.m_sec );

		WakeAllConditionVariable( &wakeMain );
		pendingChunks.clear();

		EnterCriticalSection( &m_cs.m_sec );
	}
}

HRESULT MelStreamerThread::threadPoolCallback( int ith ) noexcept
{
	SpectrogramContext& ctx = ( 0 != ith ) ? melContextsWorkers[ ith - 1 ] : melContext;

	// Figure out the slice of the chunks to generate in this thread
	const int nth = this->fftThreads;
	const int chunks = this->fftChunks;
	const int i0 = ( ith * chunks ) / nth;
	const int i1 = ( ( ith + 1 ) * chunks ) / nth;

	// Run these FFTs
	const size_t pcmChunks = tempPcm.size() / FFT_STEP;
	for( int i = i0; i < i1; i++ )
	{
		MelChunk& arr = pendingChunks[ i ];
		const float* sourcePcm = tempPcm.data() + i * FFT_STEP;
		size_t availableChunks = pcmChunks - i;
		size_t availableFloats = availableChunks * FFT_STEP;
		ctx.fft( arr, sourcePcm, availableFloats );
	}
	return S_OK;
}

HRESULT MelStreamerThread::run() noexcept
{
	HRESULT status;
	try
	{
		status = threadMain();
	}
	catch( HRESULT hr )
	{
		status = hr;
	}
	catch( const std::bad_alloc& )
	{
		status = E_OUTOFMEMORY;
	}
	catch( const std::exception& )
	{
		status = E_FAIL;
	}

	{
		Lock lk( m_cs );
		threadStatus = SUCCEEDED( status ) ? eThreadStatus::Completed : eThreadStatus::Failed;
	}

	// Especially when things fail, we want to wake the main thread up, so it's aware of the situation.
	WakeAllConditionVariable( &wakeMain );
	return status;
}

DWORD __stdcall MelStreamerThread::threadProcStatic( void* lpParameter )
{
	setCurrentThreadName( "Whisper.dll MEL Streamer Thread" );
	MelStreamerThread* p = (MelStreamerThread*)lpParameter;
	return (DWORD)p->run();
}

HRESULT MelStreamerThread::makeBuffer( size_t off, size_t len, const float** buffer, size_t& stride ) noexcept
{
	bool wakeThread = false;

	{
		Lock lock( m_cs );
		if( off < streamStartOffset )
		{
			logError( u8"MelStreamer doesn't support backwards seeks" );
			return E_UNEXPECTED;
		}

		if( off > streamStartOffset )
		{
			// The model wants to advance forward, drop now irrelevant chunks of data
			dropOldChunks( off );
			wakeThread = ( threadStatus == eThreadStatus::Working || threadStatus == eThreadStatus::Idle );
		}

		while( true )
		{
			const size_t availableMel = queueMel.size();
			if( availableMel >= len )
				break;

			const eThreadStatus ts = threadStatus;
			if( ts == eThreadStatus::Working || ts == eThreadStatus::Idle || ts == eThreadStatus::NotStarted )
			{
				// Allow the producer thread to initialize or continue filling the queue.
				WakeAllConditionVariable( &wakeBackground );
				SleepConditionVariableCS( &wakeMain, &m_cs.m_sec, INFINITE );
				continue;
			}
			if( ts == eThreadStatus::Failed )
			{
				DWORD code;
				if( GetExitCodeThread( threadHandle, &code ) )
					return (HRESULT)code;
				else
					return HRESULT_FROM_WIN32( GetLastError() );
			}
			assert( ts == eThreadStatus::Completed );
			break;
		}

		if( queueMel.size() < len )
		{
			assert( readerEof || threadStatus == eThreadStatus::Failed );
			while( queueMel.size() < len )
			{
				auto& arr = queueMel.emplace_back();
             memset( arr.data(), 0, melCount * 4 );
			}
		}

		// Produce the result
		makeTransposedBuffer( off, len );

	}	// Unlock the critical section

	stride = len;
	*buffer = outputMel.data();
	if( wakeThread )
		WakeAllConditionVariable( &wakeBackground );
	return S_OK;
}

MelStreamerThread::~MelStreamerThread()
{
	if( !threadHandle )
		return;

	{
		Lock lock( m_cs );
		if( threadStatus != eThreadStatus::Working )
			return;
		shuttingDown = true;
	}

	DWORD res = WaitForSingleObject( threadHandle, 100 );
	if( res == WAIT_OBJECT_0 )
		return;
	// TODO: log a warning
}

HRESULT MelStreamer::copyStereoPcm( size_t offset, size_t length, std::vector<StereoSample>& buffer ) const
{
	PcmQueueLock lock( pcmLock );
	if( queuePcmStereo.empty() )
		return OLE_E_BLANK;

	if( offset < streamStartOffset )
	{
		logError( u8"MelStreamer doesn't support backwards seek" );
		return E_UNEXPECTED;
	}

	// Offset relative to the first chunk on the queue
	const size_t off = offset - streamStartOffset;
	if( off >= queuePcmStereo.size() )
		return E_BOUNDS;

	// Resize the output buffer
	try
	{
		buffer.resize( length * FFT_STEP );
	}
	catch( const std::bad_alloc& )
	{
		return E_OUTOFMEMORY;
	}
	StereoSample* rdi = buffer.data();

	// Copy PCM chunks from the queue
	const size_t lengthToCopy = std::min( length, queuePcmStereo.size() - off );
	for( size_t i = 0; i < lengthToCopy; i++, rdi += FFT_STEP )
	{
		const float* rsi = queuePcmStereo[ i + off ].stereo.data();
		memcpy( rdi, rsi, 8 * FFT_STEP );
	}
	// If needed, write zeros to the tail
	if( lengthToCopy == length )
		return S_OK;
	memset( rdi, 0, ( length - lengthToCopy ) * FFT_STEP );
	return S_OK;
}