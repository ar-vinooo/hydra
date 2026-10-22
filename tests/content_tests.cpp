#include "common/common.hpp"
#include "core/horizon/filesystem/bktr.hpp"
#include "core/horizon/filesystem/content_archive.hpp"
#include "core/horizon/filesystem/disk_file.hpp"
#include "core/horizon/loader/content_package.hpp"
#include "core/horizon/loader/xci_loader.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace hydra;
using namespace hydra::horizon;
namespace {
void check(bool result, const char* message) {
    if (!result)
        throw std::runtime_error(message);
}
std::vector<u8> read(filesystem::IFile& file) {
    std::vector<u8> bytes(file.getSize());
    std::unique_ptr<ztd::io::IStream> stream(
        file.open(filesystem::FileOpenFlags::Read));
    stream->readToSpan(std::span(bytes));
    return bytes;
}
std::vector<u8> read(const std::filesystem::path& path) {
    filesystem::DiskFile file(path.string());
    return read(file);
}
template <typename F>
void rejects(F operation, const char* message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Usage: hydra-content-tests FIXTURE_DIRECTORY");
        const std::filesystem::path dir(argv[1]);
        const auto keys = filesystem::NcaFile::loadKeys(dir / "prod.keys");
        filesystem::DiskFile multi((dir / "content-multi-bucket.bin").string());
        const auto tree =
            filesystem::readBktrTable(multi, 0, multi.getSize(), 4, true);
        check(tree.end == 0x200 && tree.entries.size() == 4 &&
                  tree.entries[2].offset == 0x100,
              "Multi-bucket relocation tree was not flattened correctly");
        auto patch_source = std::make_shared<filesystem::DiskFile>(
            (dir / "content-patch.nca").string());
        bool plaintext;
        const auto header = filesystem::NcaFile::readHeader(
            *patch_source, 0, patch_source->getSize(), keys, plaintext);
        check(!plaintext, "Encrypted patch header not detected");
        auto patch = std::make_shared<filesystem::NcaFile>(
            patch_source, 0, patch_source->getSize(), header, plaintext, keys);
        check(
            read(*patch) == read(dir / "content-patch-expected.nca"),
            "CTREx generation/table decryption differs from Python reference");
        auto base = std::make_shared<filesystem::DiskFile>(
            (dir / "content-base.nca").string());
        auto merged = filesystem::applyNcaPatch(base, patch);
        filesystem::ContentArchive archive(merged.get());
        filesystem::IFile* romfs;
        check(archive.getFile("data", romfs) == filesystem::FsResult::Success,
              "Merged RomFS missing");
        const auto expected = read(dir / "content-romfs-expected.bin");
        check(read(*romfs) == expected,
              "BKTR merged bytes differ from independent reference");
        std::unique_ptr<ztd::io::IStream> stream(
            romfs->open(filesystem::FileOpenFlags::Read));
        for (u64 i = 0; i < 1000; i++) {
            const auto offset = (i * 37) % expected.size();
            const auto count =
                std::min<u64>(1 + i % 701, expected.size() - offset);
            std::vector<u8> bytes(count);
            stream->seekTo(offset);
            stream->readToSpan(std::span(bytes));
            check(std::equal(bytes.begin(), bytes.end(),
                             expected.begin() +
                                 static_cast<std::ptrdiff_t>(offset)),
                  "Random relocation-boundary read differs");
        }
        // Streams must retain base and patch after every loader/file wrapper is
        // released.
        merged.reset();
        patch.reset();
        patch_source.reset();
        base.reset();
        stream->seekTo(0);
        std::vector<u8> retained(expected.size());
        stream->readToSpan(std::span(retained));
        check(retained == expected, "Patch backing lifetime lost");
        std::cout << "PASS: encrypted CTREx, base/patch overlay, 1000 random "
                     "reads and retained streams\n";

        auto pattern = (dir / "install-tests-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back(0);
        check(mkdtemp(buffer.data()) != nullptr,
              "Cannot create isolated content store");
        const std::filesystem::path root(buffer.data());
        const auto count = [&] {
            return loader::installedContentPackages(root).size();
        };
        auto game =
            std::unique_ptr<loader::XciLoader>(loader::XciLoader::createNsp(
                (dir / "content-base.nsp").string(), root));
        check(game->getName() == "Fixture" && game->getContentVersion() == 0 &&
                  game->getAddOnContent().empty(),
              "Base game changed without installed content");
        for (const auto* name : {"content-bad-hash.nsp", "content-missing.nsp",
                                 "content-bad-tree.nsp", "content-bad-cnmt.nsp",
                                 "content-bad-range.nsp", "content-base.nsp"}) {
            rejects(
                [&] {
                    loader::installContentPackage((dir / name).string(), root);
                },
                "Malformed or base-only package was installed");
            check(count() == 0,
                  "Failed installation published partial content");
            check(std::filesystem::is_empty(root),
                  "Failed installation left staging data");
        }
        loader::installContentPackage((dir / "content-update-new.nsp").string(),
                                      root);
        loader::installContentPackage((dir / "content-update.nsp").string(),
                                      root);
        check(count() == 2, "Versioned update installs were lost");
        check(loader::installContentPackage(
                  (dir / "content-update.nsp").string(), root)
                          .find("already installed") != std::string::npos &&
                  count() == 2,
              "Duplicate install is not idempotent");
        loader::installContentPackage((dir / "content-dlc.nsp").string(), root);
        game.reset(loader::XciLoader::createNsp(
            (dir / "content-base.nsp").string(), root));
        check(game->getName() == "Updated" &&
                  game->getTitleId() == 0x0100000000002000 &&
                  game->getContentVersion() == 131072,
              "Newest matching update not applied");
        check(game->getAddOnContent().size() == 1 &&
                  game->getAddOnContent().contains(0x0100000000003001),
              "Installed DLC not registered");
        check(read(*game->getAddOnContent().at(0x0100000000003001)) ==
                  read(dir / "content-dlc-expected.bin"),
              "DLC RomFS bytes differ");
        game->extractRomFs((root / "merged.romfs").string());
        const auto extracted = read(root / "merged.romfs" / "payload.bin");
        check(extracted == std::vector<u8>(expected.begin() + 0x100,
                                           expected.begin() + 0x120),
              "Installed update does not expose merged RomFS file contents");
        std::filesystem::remove_all(root / "merged.romfs");
        const auto empty = root / "empty";
        std::filesystem::create_directory(empty);
        auto bundled =
            std::unique_ptr<loader::XciLoader>(loader::XciLoader::create(
                (dir / "content-bundled.xci").string(), empty));
        check(bundled->getName() == "Updated" &&
                  bundled->getContentVersion() == 65536,
              "Bundled update was not applied");
        auto full =
            std::unique_ptr<loader::XciLoader>(loader::XciLoader::create(
                (dir / "content-full-bundled.xci").string(), empty));
        check(full->getContentVersion() == 196608,
              "Full replacement update was mistaken for a second base game");
        loader::installContentPackage((dir / "content-dlc.nsp").string(),
                                      empty);
        rejects(
            [&] {
                std::unique_ptr<loader::XciLoader> invalid(
                    loader::XciLoader::createNsp(
                        (dir / "content-base.nsp").string(), empty));
            },
            "DLC required update version was ignored");
        std::filesystem::remove_all(root);
        std::cout << "PASS: atomic install, hash rejection, duplicate install, "
                     "newest version, bundled/full updates, DLC data and "
                     "version dependency\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
