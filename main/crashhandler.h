//=============================================================================
//  MuseScore
//  Windows local crash capture
//=============================================================================

#ifndef MSCORE_CRASHHANDLER_H
#define MSCORE_CRASHHANDLER_H

namespace Ms {

// Installs an in-process last-resort handler that keeps a local minidump and
// text record.  This is independent of Windows Error Reporting and therefore
// does not lose the dump when WER clears its temporary directory.
void installLocalCrashHandler();

}

#endif
