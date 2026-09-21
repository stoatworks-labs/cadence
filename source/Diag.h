#pragma once

#include <string>

/**
	Logging for a plugin that lives inside somebody else's process.

	Log file only: no crash handler, because a plugin loaded into Resolume
	has no business installing a process-wide signal handler, and no bundle
	command, because an effect has no UI to hang one off.

	It covers the failures that actually happen. A shader that will not
	compile -- which from the operator's side is "the effect does nothing"
	with no message anywhere, so this records which of the four it was and
	what the GL driver said. A frame ring the driver would not allocate. And
	the host's clock unit, once, because Resolume sends milliseconds and
	nothing in the FFGL header says so.
*/
namespace cadence::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace cadence::diag
