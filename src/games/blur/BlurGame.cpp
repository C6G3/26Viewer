// Blur (Bizarre Creations, 2010) — game module entry point.

#include "BafBank.hpp"
#include "ChunkFile.hpp"
#include "ModelDecoder.hpp"
#include "PakArchive.hpp"

#include "viewer/core/GameRegistry.hpp"

namespace viewer::games::blur {

namespace fs = std::filesystem;

class BlurGame final : public Game {
public:
    const GameInfo& info() const noexcept override
    {
        static const GameInfo kInfo{
            .id = "blur",
            .title = "Blur",
            .developer = "Bizarre Creations",
            .year = 2010,
            .description = "Arcade racer — browses cache/*.pak packages and loose level data.",
            .executable = "Blur.exe",
        };
        return kInfo;
    }

    bool detectInstall(const fs::path& directory) const override
    {
        std::error_code ec;
        return fs::is_regular_file(directory / "Blur.exe", ec)
               && fs::is_regular_file(directory / "cache" / "root.pak", ec);
    }

    std::vector<fs::path> suggestInstallPaths() const override
    {
        return {
            "C:/Program Files (x86)/Activision/Blur(TM)",
            "C:/Program Files (x86)/Activision/Blur",
            "C:/Program Files/Activision/Blur(TM)",
            "C:/Games/Blur",
        };
    }

    void registerContent(ContentRegistry& registry) const override
    {
        registry.addArchiveFormat(std::make_shared<PakFormat>());
        registry.addImageDecoder(std::make_shared<TextureDecoder>());
        registry.addStructureParser(std::make_shared<ChunkStructureParser>());
        registry.addMeshDecoder(std::make_shared<ModelDecoder>());
        registry.addSoundBankFormat(std::make_shared<BafFormat>());

        auto& t = registry.fileTypes();
        t.add({"pak", "Blur package", FileCategory::Archive});
        t.add({"texture", "Blur texture", FileCategory::Texture});
        t.add({"model", "Blur model", FileCategory::Model});
        t.add({"particlemesh", "Particle mesh", FileCategory::Model});
        t.add({"cycle", "Animation cycle", FileCategory::Animation});
        t.add({"skeleton", "Skeleton", FileCategory::Animation});
        t.add({"level", "Level definition", FileCategory::Level});
        t.add({"levelrender", "Level render data", FileCategory::Level});
        t.add({"levelphysics", "Level physics data", FileCategory::Level});
        t.add({"levelstream", "Level streaming archive (ARCH)", FileCategory::Level});
        t.add({"baf", "Audio bank (BANK)", FileCategory::Audio});
        t.add({"dat", "Audio/data table", FileCategory::Data});
        t.add({"ipf", "UI layout", FileCategory::Data});
        t.add({"bpr", "UI resource pack (BZR2)", FileCategory::Data});
        t.add({"pf", "Bitmap font", FileCategory::Font});
        t.add({"font", "Font", FileCategory::Font});
        t.add({"fxb", "Compiled shader effect", FileCategory::Shader});
        t.add({"loc", "Localisation table", FileCategory::Data});
        t.add({"bin", "Game data table", FileCategory::Data});
        t.add({"xt", "Data table", FileCategory::Data});
        t.add({"aid", "AI data", FileCategory::Data});
        t.add({"wbl", "Audio bank list", FileCategory::Text});
        t.add({"raw", "Raw data", FileCategory::Data});
        t.add({"sig", "Signature", FileCategory::Data});
    }
};

void registerModule(GameRegistry& registry)
{
    registry.add(std::make_unique<BlurGame>());
}

} // namespace viewer::games::blur
