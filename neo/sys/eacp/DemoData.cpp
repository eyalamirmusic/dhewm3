#include "DemoData.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Utils/File.h>
#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Network/OnlineResource/OnlineResource.h>
#include <eacp/Network/OnlineResource/OnlineResources.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "sys/platform.h"
#include "framework/Common.h"
#include "sys/sys_public.h"

// Last, and without the zlib-compatible names: those are macros - crc32,
// compress, inflate - and would rename anything of the same name included after.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

namespace dhewm3
{
namespace
{
// The installer CMake/DemoData.cmake fetched, and the same file. It pinned the
// SHA-256; OnlineResource has nothing to check a hash with, so what holds a
// download to this file here is its size and gzip's own checksum, below.
constexpr auto installerUrl = "https://files.holarse-linuxgaming.de/native/Spiele/"
                              "Doom%203/Demo/doom3-linux-1.1.1286-demo.x86.run";

// Changing it makes OnlineResource download again whatever is on disk.
constexpr auto installerVersion = "1.1.1286";

// Where the payload is. makeself 2.1.4 wrote the installer as a 373-line shell
// script, 8766 bytes of it, followed by one gzip'd tar that runs to the end of
// the file; the script's header says as much, in `offset=`head -n 373 "$0" |
// wc -c`` and `filesizes="485248552"`. Constants rather than parsed out of the
// script, because this reads the one file and no other.
constexpr std::int64_t payloadOffset = 8766;
constexpr std::int64_t payloadSize = 485248552;

// The entry the engine wants, named as the tar names it - which is also where it
// goes under the Resources folder, because demo/ is the game directory the
// engine falls back to when base/ has no default.cfg (idFileSystemLocal::Init).
constexpr auto pk4Entry = "demo/demo00.pk4";

FilePath pk4Path(const FilePath& directory)
{
    return directory / pk4Entry;
}

// The value the last `+set <name> <value>` on the command line gives the cvar,
// which is the one idCommonLocal::StartupVariable leaves it holding. Empty when
// the command line does not set it.
std::string commandLineValue(const char* name)
{
    const auto& args = Apps::getAppEnvironment().commandLineArgs;
    auto value = std::string {};

    for (std::size_t i = 1; i + 2 < args.size(); ++i)
        if (args[i] == "+set" && idStr::Icmp(args[i + 1].c_str(), name) == 0)
            value = args[i + 2];

    return value;
}

// A base/ with a pk4 in it - the retail game, however it was patched - or the
// demo's own demo/demo00.pk4.
bool hasGameData(const std::string& root)
{
    if (root.empty())
        return false;

    const auto path = toStdPath(FilePath {root});
    auto error = std::error_code {};

    if (std::filesystem::is_regular_file(path / "demo" / "demo00.pk4", error))
        return true;

    for (auto it = std::filesystem::directory_iterator {path / "base", error};
         !error && it != std::filesystem::directory_iterator {};
         it.increment(error))
    {
        const auto extension = it->path().extension();

        if (extension == ".pk4" || extension == ".PK4")
            return true;
    }

    return false;
}

// Whether the engine will find game data without being told where. It looks
// under fs_basepath and fs_cdpath, and they are taken here the way
// idFileSystemLocal::Init takes them: the command line's +set first, and
// Sys_GetPath's answer for the base path when the command line has none.
//
// This runs before common->Init, which Sys_GetPath does not mind: the warnings
// it prints on Windows go through idCommonLocal::VPrintf, which drops anything
// printed before the cvar system is up.
bool engineFindsGameData()
{
    auto basepath = commandLineValue("fs_basepath");

    if (basepath.empty())
    {
        auto path = idStr {};

        if (Sys_GetPath(PATH_BASE, path))
            basepath = path.c_str();
    }

    return hasGameData(basepath) || hasGameData(commandLineValue("fs_cdpath"));
}

// Walks a tar archive handed over in whatever pieces inflate produces, and
// writes one regular file out of it. Old-style and ustar headers agree on the
// three fields this needs - the name, the octal size and the type - and every
// other entry is stepped over unread.
class TarEntryWriter
{
public:
    TarEntryWriter(std::string wantedName, std::filesystem::path destinationPath)
        : wanted(std::move(wantedName))
        , destination(std::move(destinationPath))
    {
    }

    // False as soon as anything has gone wrong, and error() says what.
    bool write(const unsigned char* data, std::size_t size)
    {
        while (size > 0 && problem.empty() && !ended)
        {
            auto count = std::size_t {0};

            if (remaining > 0)
            {
                count = (std::size_t) std::min<std::uint64_t>(remaining, size);
                remaining -= count;

                if (out.is_open())
                    writeData(data, count);
            }
            else if (padding > 0)
            {
                count = (std::size_t) std::min<std::uint64_t>(padding, size);
                padding -= count;
            }
            else
            {
                count = std::min(sizeof(header) - filled, size);
                std::memcpy(header + filled, data, count);
                filled += count;

                if (filled == sizeof(header))
                    readHeader();
            }

            data += count;
            size -= count;
        }

        return problem.empty();
    }

    // Once the archive is over: whether the entry was in it, and written whole.
    bool finish()
    {
        if (problem.empty() && !found)
            problem = "The demo installer has no " + wanted + " in it.";
        else if (problem.empty() && out.is_open())
            problem = "The demo installer ends partway through " + wanted + ".";

        return problem.empty();
    }

    const std::string& error() const { return problem; }

private:
    void writeData(const unsigned char* data, std::size_t count)
    {
        if (!out.write((const char*) data, (std::streamsize) count))
        {
            problem = "Could not write " + wanted + ".";
            return;
        }

        if (remaining == 0)
        {
            out.close();

            if (out.fail())
                problem = "Could not write " + wanted + ".";
        }
    }

    void readHeader()
    {
        filled = 0;

        // An all-zero block is the end of the archive. What follows it is more
        // zeros, out to the record size tar rounded the archive to.
        if (std::all_of(std::begin(header),
                        std::end(header),
                        [](unsigned char byte) { return byte == 0; }))
        {
            ended = true;
            return;
        }

        const auto* nameEnd = std::find(header, header + 100, (unsigned char) 0);
        const auto name = std::string {(const char*) header, (std::size_t) (nameEnd - header)};
        const auto size = octal(header + 124, 12);
        const auto type = header[156];

        remaining = size;
        padding = (512 - size % 512) % 512;

        if (name != wanted || (type != '0' && type != '\0'))
            return;

        found = true;
        out.open(destination, std::ios::binary | std::ios::trunc);

        if (!out)
            problem = "Could not create " + wanted + ".";
        else if (size == 0)
            out.close();
    }

    static std::uint64_t octal(const unsigned char* field, std::size_t size)
    {
        auto value = std::uint64_t {0};
        auto i = std::size_t {0};

        while (i < size && field[i] == ' ')
            ++i;

        for (; i < size && field[i] >= '0' && field[i] <= '7'; ++i)
            value = value * 8 + (std::uint64_t) (field[i] - '0');

        return value;
    }

    std::string wanted;
    std::filesystem::path destination;
    std::ofstream out;
    std::string problem;

    unsigned char header[512] {};
    std::size_t filled = 0;
    std::uint64_t remaining = 0;
    std::uint64_t padding = 0;
    bool found = false;
    bool ended = false;
};

std::uint32_t littleEndian32(const unsigned char* bytes)
{
    return (std::uint32_t) bytes[0] | (std::uint32_t) bytes[1] << 8
           | (std::uint32_t) bytes[2] << 16 | (std::uint32_t) bytes[3] << 24;
}

// Inflates the installer's payload, walks the tar inside it for pk4Entry and
// writes that to `destination`, and holds the whole of the inflated stream to
// the CRC-32 and length gzip stores after it - which is what stands between a
// corrupt download and a corrupt pk4. Empty when the pk4 is written, and what
// went wrong when it is not.
std::string unpackPayload(const FilePath& installer,
                          const std::filesystem::path& destination,
                          std::atomic<std::int64_t>& progress,
                          const std::atomic<bool>& cancelled)
{
    const auto source = toStdPath(installer);
    auto error = std::error_code {};
    const auto size = std::filesystem::file_size(source, error);

    if (error || size != (std::uintmax_t) (payloadOffset + payloadSize))
        return "The download is not the size of the demo installer.";

    auto in = std::ifstream {source, std::ios::binary};
    in.seekg(payloadOffset);

    // RFC 1952's member header, with none of the optional fields: gzip read this
    // one from a pipe, so there was no file name to store.
    unsigned char header[10] {};

    if (!in.read((char*) header, sizeof(header)) || header[0] != 0x1f
        || header[1] != 0x8b || header[2] != 8 || header[3] != 0)
        return "The download is not the demo installer.";

    auto stream = mz_stream {};

    // Negative window bits: a raw deflate stream, since the header above was
    // gzip's rather than the zlib one miniz would otherwise expect.
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return "Could not start inflating the demo installer.";

    struct InflateEnd
    {
        mz_stream& stream;
        ~InflateEnd() { mz_inflateEnd(&stream); }
    } inflateEnd {stream};

    auto tar = TarEntryWriter {pk4Entry, destination};
    auto input = std::vector<unsigned char>(1 << 20);
    auto output = std::vector<unsigned char>(1 << 20);
    auto checksum = (mz_ulong) MZ_CRC32_INIT;
    auto length = std::uint64_t {0};
    auto status = (int) MZ_OK;

    while (status != MZ_STREAM_END)
    {
        if (cancelled)
            return "Cancelled.";

        if (stream.avail_in == 0)
        {
            in.read((char*) input.data(), (std::streamsize) input.size());

            if (in.gcount() <= 0)
                return "The demo installer ends partway through.";

            stream.next_in = input.data();
            stream.avail_in = (unsigned int) in.gcount();
        }

        stream.next_out = output.data();
        stream.avail_out = (unsigned int) output.size();
        status = mz_inflate(&stream, MZ_NO_FLUSH);

        if (status != MZ_OK && status != MZ_STREAM_END && status != MZ_BUF_ERROR)
        {
            const auto* message = mz_error(status);
            return std::string {"The demo installer is corrupt ("}
                   + (message != nullptr ? message : "inflate failed") + ").";
        }

        const auto produced = output.size() - stream.avail_out;
        checksum = mz_crc32(checksum, output.data(), produced);
        length += produced;

        if (!tar.write(output.data(), produced))
            return tar.error();

        progress = (std::int64_t) stream.total_in;
    }

    // The eight bytes after the deflate stream: the CRC-32 of everything it
    // inflated to, then its length modulo 2^32.
    unsigned char trailer[8] {};
    const auto buffered = std::min<std::size_t>(stream.avail_in, sizeof(trailer));
    std::memcpy(trailer, stream.next_in, buffered);

    if (buffered < sizeof(trailer)
        && !in.read((char*) trailer + buffered,
                    (std::streamsize) (sizeof(trailer) - buffered)))
        return "The demo installer ends before its checksum.";

    if (littleEndian32(trailer) != (std::uint32_t) checksum
        || littleEndian32(trailer + 4) != (std::uint32_t) length)
        return "The download is corrupt: it does not match its own checksum.";

    if (!tar.finish())
        return tar.error();

    return {};
}

// unpackPayload into a .part beside the pk4, renamed into place only once it is
// whole, so a pk4 the engine finds is never half of one however the unpack
// ended - a quit halfway through included.
std::string unpack(const FilePath& installer,
                   const FilePath& target,
                   std::atomic<std::int64_t>& progress,
                   const std::atomic<bool>& cancelled)
{
    const auto destination = toStdPath(target);
    auto part = destination;
    part += ".part";

    auto error = std::error_code {};
    std::filesystem::create_directories(destination.parent_path(), error);

    auto result = unpackPayload(installer, part, progress, cancelled);

    if (result.empty())
    {
        std::filesystem::rename(part, destination, error);

        if (error)
            result = "Could not move demo00.pk4 into place: " + error.message();
    }

    if (!result.empty())
        std::filesystem::remove(part, error);

    return result;
}
} // namespace

DemoData::DemoData()
{
    if (engineFindsGameData())
        return;

    usingDemo = true;
    directory = OnlineResources::get().getDirectory();

    if (File {pk4Path(directory)}.exists())
        return;

    startDownload();
}

DemoData::~DemoData()
{
    // A quit mid-unpack. The .part it leaves is overwritten by the next start,
    // which finds the installer still on disk and unpacks it again.
    cancelled = true;

    if (unpacker.joinable())
        unpacker.join();
}

void DemoData::update()
{
    if (current != Stage::unpacking || !unpackFinished)
        return;

    unpacker.join();

    // Deleted however the unpack went. After one that worked it is 485 MB of
    // nothing, and after one that did not it is most likely a bad download,
    // which only a new one fixes.
    installer->remove();

    if (!unpackError.empty())
    {
        fail(unpackError);
        return;
    }

    Sys_Printf("Unpacked the Doom 3 demo to %s\n", pk4Path(directory).c_str());
    current = Stage::ready;
}

std::int64_t DemoData::bytesDone() const
{
    if (current == Stage::unpacking)
        return unpackedBytes;

    if (current == Stage::downloading && installer != nullptr)
        return installer->progress().bytesReceived;

    return 0;
}

std::int64_t DemoData::bytesTotal() const
{
    if (current == Stage::unpacking)
        return payloadSize;

    if (current == Stage::downloading && installer != nullptr)
        return installer->progress().totalBytes;

    return -1;
}

void DemoData::retry()
{
    if (current == Stage::failed)
        startDownload();
}

std::vector<std::string> DemoData::engineArguments() const
{
    if (!usingDemo)
        return {};

    return {"+set", "fs_cdpath", directory.str()};
}

void DemoData::startDownload()
{
    Sys_Printf("No game data found - downloading the Doom 3 demo to %s\n",
               directory.c_str());

    current = Stage::downloading;
    problem.clear();

    if (installer == nullptr)
    {
        auto info = OnlineResource::Info {};
        info.name = "the Doom 3 demo";
        info.url = installerUrl;
        info.version = installerVersion;

        // Trusted once it is on disk: the only copy there ever is is one an
        // unpack is about to delete, and asking the server about it would be a
        // round trip to learn nothing.
        installer = std::make_unique<OnlineResource>(
            std::move(info), directory, OnlineResource::Freshness::trust);
    }

    // Destroying the installer abandons this, so it never calls into a DemoData
    // that has gone.
    installer->start().then(
        [this](OnlineResource::Result result)
        {
            if (result.ok)
                startUnpacking(result.path);
            else if (result.cancelled)
                fail("The download was cancelled.");
            else
                fail("The download failed: " + result.error);
        });
}

void DemoData::startUnpacking(const FilePath& installerPath)
{
    current = Stage::unpacking;
    unpackedBytes = 0;
    unpackFinished = false;
    unpackError.clear();

    unpacker = std::thread {
        [this, installerPath]
        {
            unpackError =
                unpack(installerPath, pk4Path(directory), unpackedBytes, cancelled);
            unpackFinished = true;
        }};
}

void DemoData::fail(const std::string& reason)
{
    Sys_Printf("The Doom 3 demo could not be fetched: %s\n", reason.c_str());

    current = Stage::failed;
    problem = reason;
}
} // namespace dhewm3
