/*
===========================================================================

dhewm 3 on eacp - the free Doom 3 demo, for a machine with no game data.

CMake/DemoData.cmake used to download it into the build tree at configure
time, which made that build tree runnable and nothing else: the app copied
anywhere had no data again. This is the same file, fetched by the app itself
the first time it starts without game data. eacp's OnlineResource does the
transfer and keeps it in the app's own Resources folder.

The demo ships as a makeself installer - a shell script with a gzip'd tar
appended - and nothing here runs it. It is read as bytes: the gzip stream is
inflated with the miniz the engine already reads pk4s with, and the one entry
the engine wants, demo/demo00.pk4, is written out of the tar as it goes past.
The installer is deleted afterwards, so what stays on disk is the pk4.

The engine is told about none of this except through its own cvars: the
Resources folder is handed to it as fs_cdpath, the search path that used to be
the CD, and its existing fallback from base/ to demo/ does the rest.

===========================================================================
*/

#pragma once

#include <eacp/Core/Utils/FilePath.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace eacp
{
class OnlineResource;
}

namespace dhewm3
{
using namespace eacp;

class DemoData
{
public:
    enum class Stage
    {
        downloading,
        unpacking,
        failed,
        ready
    };

    // Looks for game data where the engine will, and starts fetching the demo
    // when there is none. Ready at once when there is.
    DemoData();
    ~DemoData();

    DemoData(const DemoData&) = delete;
    DemoData& operator=(const DemoData&) = delete;

    // Once a refresh, on the main thread: collects an unpack that has finished.
    void update();

    Stage stage() const { return current; }

    // How far through the stage it is: bytes received of the download, or
    // bytes of the installer's compressed payload read while unpacking. The
    // total is -1 while the server has not said.
    std::int64_t bytesDone() const;
    std::int64_t bytesTotal() const;

    // Why it failed, while it has.
    const std::string& error() const { return problem; }

    // Starts over after a failure, and does nothing otherwise.
    void retry();

    // What goes in front of the engine's command line: fs_cdpath pointed at
    // the Resources folder when the demo there is what the engine will run on,
    // and nothing when the engine found game data of its own. In front, so a
    // +set the user typed still wins.
    std::vector<std::string> engineArguments() const;

private:
    void startDownload();
    void startUnpacking(const FilePath& installerPath);
    void fail(const std::string& reason);

    Stage current = Stage::ready;
    bool usingDemo = false;
    FilePath directory;
    std::string problem;

    std::unique_ptr<OnlineResource> installer;

    std::thread unpacker;
    std::atomic<std::int64_t> unpackedBytes {0};
    std::atomic<bool> unpackFinished {false};
    std::atomic<bool> cancelled {false};

    // Written by the unpacker and read once it has been joined.
    std::string unpackError;
};
} // namespace dhewm3
