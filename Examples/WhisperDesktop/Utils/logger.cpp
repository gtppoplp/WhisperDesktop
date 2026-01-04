#include "stdafx.h"
#include "logger.h"
#include "miscUtils.h"

namespace
{
	using namespace Whisper;

	// Terminal color map. 10 colors grouped in ranges [0.0, 0.1, ..., 0.9]
	// Lowest is red, middle is yellow, highest is green.
	static const std::array<const char*, 10> k_colors =
	{
		"\033[38;5;196m", "\033[38;5;202m", "\033[38;5;208m", "\033[38;5;214m", "\033[38;5;220m",
		"\033[38;5;226m", "\033[38;5;190m", "\033[38;5;154m", "\033[38;5;118m", "\033[38;5;82m",
	};

	static int colorIndex( const sToken& tok )
	{
		const float p = tok.probability;
		const float p3 = p * p * p;
		int col = (int)( p3 * float( k_colors.size() ) );
		col = std::max( 0, std::min( (int)k_colors.size() - 1, col ) );
		return col;
	}
}

void printTime( CStringA& rdi, Whisper::sTimeSpan time, bool comma )
{
	Whisper::sTimeSpanFields fields = time;
	const uint32_t hours = fields.days * 24 + fields.hours;
	const char separator = comma ? ',' : '.';
	rdi.AppendFormat( "%02d:%02d:%02d%c%03d",
		(int)hours,
		(int)fields.minutes,
		(int)fields.seconds,
		separator,
		fields.ticks / 10'000 );
}

HRESULT logNewSegments( const iTranscribeResult* results, size_t newSegments, bool printSpecial )
{
	sTranscribeLength length;
	CHECK( results->getSize( length ) );

	const size_t len = length.countSegments;
	size_t i = len - newSegments;

	const sSegment* const segments = results->getSegments();

	CStringA str;
	for( ; i < len; i++ )
	{
		const sSegment& seg = segments[ i ];
		str = "[";
		printTime( str, seg.time.begin );
		str += " --> ";
		printTime( str, seg.time.end );
		str += "]  ";

		// Use segment text directly to avoid issues with split UTF-8 sequences
		// When Whisper processes Chinese text, it may split UTF-8 bytes across tokens
		if( seg.text != nullptr )
		{
			str += seg.text;
		}
		
		logInfo( u8"%s", cstr( str ) );
	}

	return S_OK;
}
