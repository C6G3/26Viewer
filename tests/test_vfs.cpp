#include "TestUtil.hpp"

#include "viewer/core/Hash.hpp"
#include "viewer/core/RandomAccessFile.hpp"
#include "viewer/core/Vfs.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace viewer;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) : path(fs::temp_directory_path() / ("26viewer_test_" + name))
    {
        fs::remove_all(path);
        fs::create_directories(path / "sub");
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("SHA-1 test vectors")
{
    CHECK(hash::toHex(hash::sha1(asBytes(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hash::toHex(hash::sha1(asBytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hash::toHex(hash::sha1(asBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))
          == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("Read-only files can be replaced and stay read-only")
{
    TempDir dir("readonly");
    const fs::path file = dir.path / "sub" / "locked.txt";
    REQUIRE(writeFile(file, asBytes("original")));
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::remove);

    ContentRegistry content;
    Vfs vfs;
    vfs.mountDirectory(dir.path, content);
    const VfsNode& node = *vfs.root().children().at(0)->children().at(0);
    REQUIRE(ensureBackup(file)); // works on read-only files too
    REQUIRE(vfs.replace(node, asBytes("changed")));

    const auto now = readFile(file).value();
    CHECK(std::string(reinterpret_cast<const char*>(now.data()), now.size()) == "changed");
    CHECK((fs::status(file).permissions() & fs::perms::owner_write) == fs::perms::none);
    REQUIRE(restoreBackup(file));

    // Allow cleanup.
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::add);
    fs::permissions(backupPathFor(file), fs::perms::owner_write, fs::perm_options::add);
}

TEST_CASE("Loose files: mount, replace with backup, restore")
{
    TempDir dir("vfs");
    REQUIRE(writeFile(dir.path / "sub" / "a.txt", asBytes("original")));

    ContentRegistry content;
    registerBuiltinContent(content);
    Vfs vfs;
    const auto report = vfs.mountDirectory(dir.path, content);
    CHECK(report.files == 1);

    const VfsNode& sub = *vfs.root().children().at(0);
    const VfsNode& file = *sub.children().at(0);
    CHECK(file.virtualPath() == "sub/a.txt");
    REQUIRE(Vfs::canReplace(file));
    CHECK_FALSE(Vfs::canReplace(sub));

    // Replacing never backs up by itself; that's the user's explicit choice.
    REQUIRE(vfs.replace(file, asBytes("unbacked")));
    CHECK_FALSE(fs::exists(dir.path / "sub" / "a.txt.bak"));
    REQUIRE(writeFile(dir.path / "sub" / "a.txt", asBytes("original")));

    REQUIRE(ensureBackup(dir.path / "sub" / "a.txt"));
    auto replaced = vfs.replace(file, asBytes("new contents"));
    REQUIRE(replaced);
    CHECK(replaced->backup == dir.path / "sub" / "a.txt.bak");
    CHECK(readFile(dir.path / "sub" / "a.txt").value() == ByteBuffer(asBytes("new contents").begin(), asBytes("new contents").end()));

    // Backing up again keeps the first backup (the true original).
    REQUIRE(vfs.replace(file, asBytes("third")));
    REQUIRE(ensureBackup(dir.path / "sub" / "a.txt"));
    const auto backup = readFile(dir.path / "sub" / "a.txt.bak").value();
    CHECK(std::string(reinterpret_cast<const char*>(backup.data()), backup.size()) == "original");

    // Restore must work while another part of the app still has the file open.
    auto openHandle = RandomAccessFile::open(dir.path / "sub" / "a.txt");
    REQUIRE(openHandle);
    REQUIRE(restoreBackup(dir.path / "sub" / "a.txt"));
    openHandle->reset();
    const auto restored = readFile(dir.path / "sub" / "a.txt").value();
    CHECK(std::string(reinterpret_cast<const char*>(restored.data()), restored.size()) == "original");
}
