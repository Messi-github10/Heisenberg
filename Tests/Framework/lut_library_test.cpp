#include <Compositing/Grade/LutLibrary.hpp>
#include <Compositing/CompositionBuilder.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <type_traits>

namespace {
using namespace heisenberg::grade;
namespace fs = std::filesystem;

constexpr const char* identity =
    "LUT_3D_SIZE 2\n"
    "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
constexpr const char* identityHash = "c8bce4299c8606d5ca59a4724f46e484e430c42d506cfc2a3f30bbe84d5199cc";

class LutLibraryTest : public testing::Test {
protected:
    fs::path directory;
    void SetUp() override {
        static std::atomic<unsigned> sequence{0};
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = fs::temp_directory_path() /
                ("heisenberg-lut-test-" + std::to_string(random()) + "-" + std::to_string(sequence++));
            if (fs::create_directory(candidate)) { directory = std::move(candidate); break; }
        }
        ASSERT_FALSE(directory.empty());
    }
    void TearDown() override {
        LutLibrary::setDefaultRoot({});
        std::error_code ignored;
        fs::remove_all(directory,ignored);
    }
    fs::path write(const fs::path& name, const std::string& text) {
        auto path = directory / name;
        fs::create_directories(path.parent_path());
        std::ofstream stream(path,std::ios::binary | std::ios::trunc);
        stream.write(text.data(),static_cast<std::streamsize>(text.size()));
        stream.close();
        EXPECT_TRUE(stream.good());
        return path;
    }
    fs::path root() const { return directory / "app-owned-library"; }
    bool noStagingFiles() const {
        for (const auto& entry : fs::directory_iterator(root()))
            if (entry.path().filename().string().starts_with(".lut-import-")) return false;
        return true;
    }
};

TEST_F(LutLibraryTest, ContentAddressMatchesIndependentSha256AndIsAnImmutableSnapshot) {
    static_assert(std::is_same_v<LutLibrary::Snapshot::element_type,const LutAsset>);
    LutLibrary library(root());
    std::string error = "stale";
    const auto asset = library.importFile(write("identity.cube",identity),&error);
    ASSERT_NE(asset,nullptr) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(asset->id,identityHash);
    EXPECT_EQ(asset->hash,asset->id);
    EXPECT_EQ(asset->resourcePath,root() / (asset->id+".cube"));
    EXPECT_EQ(asset->displayName,"identity");
    EXPECT_EQ(asset->cubeText,identity);
    EXPECT_EQ(asset->metadata.size,2u);
    EXPECT_EQ(asset->metadata.domainMin,(std::array<float,3>{0,0,0}));
    EXPECT_EQ(asset->metadata.domainMax,(std::array<float,3>{1,1,1}));
    ASSERT_EQ(asset->samples.size(),8u);
    EXPECT_EQ(asset->samples[1],(std::array<float,3>{1,0,0}));
    EXPECT_EQ(asset->samples[2],(std::array<float,3>{0,1,0}));
    EXPECT_EQ(library.resolve(asset->id),asset);
    EXPECT_TRUE(noStagingFiles());
}

TEST_F(LutLibraryTest, CommentsWhitespaceQuotedTitleAndInputDomainPreserveOutputValues) {
    LutLibrary library(root());
    const std::string text =
        "\xef\xbb\xbf  # comment\r\n\tTITLE \"Creative # warm\" # comment\r\n"
        "DOMAIN_MIN -1 +2 0.25 # input bounds\n"
        " DOMAIN_MAX 2 4 8\n\tLUT_3D_SIZE 2 # samples\n"
        "-2.5 4.25 0.125\n1e-2 +2E+0 -3\n0 1 0\n1 1 0 # sample\n"
        "0 0 1\n1 0 1\n0 1 1\n 9 10 11\t";
    std::string error;
    auto asset = library.importFile(write("look.CUBE",text),&error);
    ASSERT_NE(asset,nullptr) << error;
    EXPECT_EQ(asset->displayName,"Creative # warm");
    EXPECT_EQ(asset->metadata.title,"Creative # warm");
    EXPECT_EQ(asset->metadata.domainMin,(std::array<float,3>{-1,2,0.25f}));
    EXPECT_EQ(asset->metadata.domainMax,(std::array<float,3>{2,4,8}));
    EXPECT_EQ(asset->samples.front(),(std::array<float,3>{-2.5f,4.25f,0.125f}));
    EXPECT_EQ(asset->samples.back(),(std::array<float,3>{9,10,11}));
    EXPECT_EQ(asset->cubeText,text);
    LutLibrary reopened(root());
    auto resolved = reopened.resolve(asset->id,&error);
    ASSERT_NE(resolved,nullptr) << error;
    EXPECT_EQ(resolved->samples,asset->samples);
    EXPECT_EQ(resolved->metadata.domainMin,asset->metadata.domainMin);
    EXPECT_EQ(resolved->cubeText,text);
}

TEST_F(LutLibraryTest, RejectsMalformedDimensionsCountsShapersAndDirectivesBeforePublishing) {
    const std::string invalid[] = {
        "", "# comment only\n", "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n",
        std::string("LUT_1D_SIZE 2\n")+identity,
        std::string("LUT_1D_INPUT_RANGE 0 1\n")+identity,
        std::string("LUT_3D_INPUT_RANGE 0 1\n")+identity,
        "LUT_3D_SIZE 1\n", "LUT_3D_SIZE 129\n", "LUT_3D_SIZE -2\n",
        "LUT_3D_SIZE 2.0\n", "LUT_3D_SIZE 4294967296\n", "LUT_3D_SIZE 2 extra\n",
        std::string("LUT_3D_SIZE 2\n")+identity,
        "LUT_3D_SIZE 2\n0 0 0\n",
        std::string(identity)+"1 1 1\n",
        std::string(identity)+"LUT_1D_SIZE 2\n",
        std::string("0 0 0\n")+identity,
        std::string("TITLE unquoted\n")+identity,
        std::string("TITLE \"unterminated\n")+identity,
        std::string("TITLE \"ok\" garbage\n")+identity,
        std::string("TITLE \"a\"\nTITLE \"b\"\n")+identity,
        std::string(identity)+"TITLE \"late\"\n",
        std::string("UNKNOWN_METADATA 3\n")+identity,
        std::string(identity)+"DOMAIN_MIN 0 0 0\n",
        std::string("DOMAIN_MIN 0 0 0\nDOMAIN_MIN 0 0 0\n")+identity,
        std::string("DOMAIN_MIN 0 0 0 extra\n")+identity,
        std::string("DOMAIN_MAX 1 1\n")+identity,
        std::string("DOMAIN_MAX 1 0 1\n")+identity,
        std::string("DOMAIN_MIN 0 2 0\nDOMAIN_MAX 1 1 1\n")+identity,
        std::string("DOMAIN_MIN nan 0 0\n")+identity,
        std::string("DOMAIN_MAX inf 1 1\n")+identity,
        std::string("DOMAIN_MAX 1e99 1 1\n")+identity,
        std::string("LUT_3D_SIZE 2\nnan 0 0\n")+identity,
        std::string("LUT_3D_SIZE 2\n1e99 0 0\n")+identity,
        std::string("LUT_3D_SIZE 2\n0 0\n")+identity,
        std::string("LUT_3D_SIZE 2\n0 0 0 0\n")+identity,
        std::string("LUT_3D_SIZE 2\n1x 0 0\n")+identity,
        std::string("LUT_3D_SIZE 2\n+-1 0 0\n")+identity,
        std::string("LUT_3D_SIZE 2\n0")+std::string(1,'\0')+" 0 0\n"+identity,
        std::string("#")+std::string(LutLibrary::MaxLineBytes,'x')+"\n"+identity
    };
    LutLibrary library(root());
    for (std::size_t i = 0; i < std::size(invalid); ++i) {
        std::string error;
        EXPECT_EQ(library.importFile(write("bad.cube",invalid[i]),&error),nullptr) << i;
        EXPECT_FALSE(error.empty()) << i;
        EXPECT_FALSE(fs::exists(root())) << i;
    }
}

TEST_F(LutLibraryTest, RejectsNonfiniteAndMalformedSampleNumbersEvenWithTheRightCount) {
    LutLibrary library(root());
    for (const auto* bad : {"nan", "NaN", "inf", "-inf", "1e100", "0x1p0", "1,5", "1suffix", "+-1", "--1"}) {
        auto text = std::string(identity);
        text.replace(text.find("0 0 0"),1,bad);
        std::string error;
        EXPECT_EQ(library.importFile(write("bad.cube",text),&error),nullptr) << bad;
        EXPECT_FALSE(error.empty()) << bad;
    }
}

TEST_F(LutLibraryTest, FileCapRejectsOversizedSparseFileWithoutReadingIt) {
    LutLibrary library(root());
    const auto path = write("too-large.cube",identity);
    fs::resize_file(path,LutLibrary::MaxFileBytes+1);
    std::string error;
    EXPECT_EQ(library.importFile(path,&error),nullptr);
    EXPECT_NE(error.find("limit"),std::string::npos);
    EXPECT_FALSE(fs::exists(root()));
}

TEST_F(LutLibraryTest, AcceptsMaximumDimensionWithExactCount) {
    const std::size_t count = static_cast<std::size_t>(LutLibrary::MaxSize)*LutLibrary::MaxSize*LutLibrary::MaxSize;
    std::string text = "LUT_3D_SIZE 128\n";
    text.reserve(text.size()+count*6);
    for (std::size_t i = 0; i < count; ++i) text += "0 0 0\n";
    LutLibrary library(root());
    std::string error;
    auto asset = library.importFile(write("large.cube",text),&error);
    ASSERT_NE(asset,nullptr) << error;
    EXPECT_EQ(asset->metadata.size,128u);
    EXPECT_EQ(asset->samples.size(),count);
}

TEST_F(LutLibraryTest, DedupeSharesSnapshotAndSameFilenameWithDifferentContentIsIndependent) {
    LutLibrary library(root());
    auto first = library.importFile(write("a/same.cube",identity));
    ASSERT_NE(first,nullptr);
    auto duplicate = library.importFile(write("b/renamed.cube",identity));
    ASSERT_NE(duplicate,nullptr);
    EXPECT_EQ(duplicate,first);
    auto text = std::string(identity);
    text.replace(text.find("0 0 0"),1,"2");
    auto second = library.importFile(write("b/same.cube",text));
    ASSERT_NE(second,nullptr);
    EXPECT_NE(first->id,second->id);
    EXPECT_EQ(first->displayName,second->displayName);
    EXPECT_NE(first->resourcePath,second->resourcePath);
    EXPECT_TRUE(fs::exists(first->resourcePath));
    EXPECT_TRUE(fs::exists(second->resourcePath));
    EXPECT_TRUE(noStagingFiles());
}

TEST_F(LutLibraryTest, SourceCanDisappearAndImportDoesNotCreateProjectCopies) {
    LutLibrary library(root());
    const auto source = write("external/look.cube",identity);
    auto asset = library.importFile(source);
    ASSERT_NE(asset,nullptr);
    fs::remove_all(source.parent_path());
    library.invalidate();
    auto reloaded = library.resolve(asset->id);
    ASSERT_NE(reloaded,nullptr);
    EXPECT_EQ(reloaded->cubeText,identity);
    EXPECT_EQ(reloaded->id,asset->id);
    EXPECT_FALSE(fs::exists(directory / "project"));
    EXPECT_EQ(static_cast<std::size_t>(std::distance(fs::directory_iterator(root()),fs::directory_iterator{})),1u);
}

TEST_F(LutLibraryTest, ExplicitInvalidationRechecksCorruptAndMissingFilesWhileHeldSnapshotIsStable) {
    LutLibrary library(root());
    auto asset = library.importFile(write("source.cube",identity));
    ASSERT_NE(asset,nullptr);
    auto different = std::string(identity);
    different.replace(different.find("0 0 0"),1,"2");
    write(asset->resourcePath,different);
    EXPECT_EQ(library.resolve(asset->id),asset); // Explicit immutable-root contract.
    EXPECT_EQ(asset->cubeText,identity);
    EXPECT_EQ(asset->samples.front(),(std::array<float,3>{0,0,0}));
    library.invalidate(asset->id);
    std::string error;
    EXPECT_EQ(library.resolve(asset->id,&error),nullptr);
    EXPECT_NE(error.find("hash"),std::string::npos);
    EXPECT_EQ(library.importFile(directory / "source.cube",&error),nullptr);
    std::ifstream untouched(asset->resourcePath,std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(untouched),{}),different);
    untouched.close();
    fs::remove(asset->resourcePath);
    EXPECT_EQ(library.resolve(asset->id,&error),nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(asset->cubeText,identity);
}

TEST_F(LutLibraryTest, DoesNotOverwriteAnyExistingDestinationOrUserFile) {
    fs::create_directories(root());
    const auto destination = write(root() / (std::string(identityHash)+".cube"),"user-owned content");
    const auto unrelated = write(root() / "user.cube","unrelated");
    LutLibrary library(root());
    std::string error;
    EXPECT_EQ(library.importFile(write("look.cube",identity),&error),nullptr);
    EXPECT_FALSE(error.empty());
    std::ifstream stream(destination,std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream),{}),"user-owned content");
    EXPECT_TRUE(fs::exists(unrelated));
    EXPECT_TRUE(noStagingFiles());
    library.invalidate();
    EXPECT_TRUE(fs::exists(destination));
    EXPECT_TRUE(fs::exists(unrelated));
}

TEST_F(LutLibraryTest, InvalidIdsCannotEscapeRootAndOnlyCubeExtensionsAreImported) {
    LutLibrary library(root());
    for (const auto* id : {"", "../outside", "sha256:abc", "ABCDEF", "../../../../look.cube"}) {
        std::string error;
        EXPECT_EQ(library.resolve(id,&error),nullptr);
        EXPECT_FALSE(error.empty());
    }
    EXPECT_EQ(library.resolve(std::string(64,'A')),nullptr);
    EXPECT_EQ(library.resolve(std::string(64,'g')),nullptr);
    EXPECT_EQ(library.resolve(std::string(64,'0')),nullptr);
    EXPECT_EQ(library.importFile(write("look.3dl",identity)),nullptr);
    EXPECT_EQ(library.importFile(directory / "missing.cube"),nullptr);
}

TEST_F(LutLibraryTest, ConfiguredRootServiceHasNoImplicitDefaultAndRootChangesKeepSnapshotsAlive) {
    LutLibrary::setDefaultRoot({});
    EXPECT_TRUE(LutLibrary::configuredRoot().empty());
    std::string error;
    EXPECT_EQ(LutLibrary::resolveConfigured(identityHash,&error),nullptr);
    EXPECT_FALSE(error.empty());
    LutLibrary empty({});
    EXPECT_EQ(empty.importFile(write("source.cube",identity)),nullptr);
    LutLibrary first(root());
    auto asset = first.importFile(directory / "source.cube");
    ASSERT_NE(asset,nullptr);
    LutLibrary::setDefaultRoot(root());
    EXPECT_EQ(LutLibrary::configuredRoot(),root());
    auto configured = LutLibrary::resolveConfigured(asset->id);
    ASSERT_NE(configured,nullptr);
    EXPECT_EQ(LutLibrary::resolveConfigured(asset->id),configured);
    fs::remove(asset->resourcePath);
    EXPECT_EQ(LutLibrary::resolveConfigured(asset->id),configured);
    LutLibrary::invalidateConfigured(asset->id);
    EXPECT_EQ(LutLibrary::resolveConfigured(asset->id),nullptr);
    LutLibrary::setDefaultRoot(directory / "other-library");
    EXPECT_EQ(LutLibrary::resolveConfigured(asset->id),nullptr);
    EXPECT_EQ(configured->cubeText,identity);
    EXPECT_EQ(configured->samples.size(),8u);
}

TEST_F(LutLibraryTest, ConcurrentImportsAndResolvesShareOneSnapshotWithoutTemporaryFiles) {
    LutLibrary library(root());
    auto source = write("look.cube",identity);
    std::vector<std::future<LutLibrary::Snapshot>> futures;
    for (int i = 0; i < 12; ++i)
        futures.push_back(std::async(std::launch::async,[library,source]() mutable { return library.importFile(source); }));
    auto asset = futures.front().get();
    ASSERT_NE(asset,nullptr);
    for (std::size_t i = 1; i < futures.size(); ++i) EXPECT_EQ(futures[i].get(),asset);
    library.invalidate();
    futures.clear();
    for (int i = 0; i < 12; ++i)
        futures.push_back(std::async(std::launch::async,[library] { return library.resolve(identityHash); }));
    auto resolved = futures.front().get();
    ASSERT_NE(resolved,nullptr);
    for (std::size_t i = 1; i < futures.size(); ++i) EXPECT_EQ(futures[i].get(),resolved);
    EXPECT_TRUE(noStagingFiles());
}

TEST_F(LutLibraryTest, IndependentLibrariesPublishConcurrentlyWithoutReplacingContent) {
    const auto source = write("look.cube",identity);
    std::vector<std::future<LutLibrary::Snapshot>> futures;
    for (int i = 0; i < 8; ++i)
        futures.push_back(std::async(std::launch::async,[this,source] { return LutLibrary(root()).importFile(source); }));
    for (auto& future : futures) {
        const auto asset = future.get();
        ASSERT_NE(asset,nullptr);
        EXPECT_EQ(asset->id,identityHash);
        EXPECT_EQ(asset->cubeText,identity);
    }
    EXPECT_TRUE(noStagingFiles());
}

TEST_F(LutLibraryTest, CompositionPlanPinsValidatedCpuSnapshotAcrossInvalidationAndFileChange) {
    using namespace heisenberg;
    LutLibrary library(root());
    auto asset = library.importFile(write("source.cube",identity));
    ASSERT_NE(asset,nullptr);
    LutLibrary::setDefaultRoot(root());
    Timeline timeline;
    timeline.id = "timeline";
    Track track;
    track.id = "video";
    Clip clip;
    clip.id = "clip"; clip.mediaRef = "offline"; clip.durationFrames = 10;
    clip.effects = JsonValue::array({{{"type","video.colorGrade"},{"params",{{"lut",{
        {"id",asset->id},{"name","identity"},{"domain","rec709"},{"intensity",{{"value",1.0}}}
    }}}}}});
    track.clips.push_back(clip);
    timeline.tracks.push_back(track);
    CompositionPlan plan;
    std::string error;
    ASSERT_TRUE(CompositionBuilder::build(timeline,MediaManifest{},"",plan,&error)) << error;
    ASSERT_EQ(plan.gradeLuts.size(),1u);
    EXPECT_TRUE(plan.gradeLutErrors.empty());
    const auto pinned = plan.gradeLuts.at(asset->id);
    EXPECT_EQ(pinned->cubeText,identity);
    auto changed = std::string(identity);
    changed.replace(changed.find("0 0 0"),1,"2");
    write(asset->resourcePath,changed);
    LutLibrary::invalidateConfigured();
    EXPECT_EQ(LutLibrary::resolveConfigured(asset->id),nullptr);
    EXPECT_EQ(plan.gradeLuts.at(asset->id),pinned);
    EXPECT_EQ(pinned->samples.front(),(std::array<float,3>{0,0,0}));
    CompositionPlan corruptPlan;
    ASSERT_TRUE(CompositionBuilder::build(timeline,MediaManifest{},"",corruptPlan,&error)) << error;
    EXPECT_TRUE(corruptPlan.gradeLuts.empty());
    ASSERT_EQ(corruptPlan.gradeLutErrors.size(),1u);
    EXPECT_NE(corruptPlan.gradeLutErrors.at(asset->id).find("hash"),std::string::npos);
    fs::remove(asset->resourcePath);
    CompositionPlan missingPlan;
    ASSERT_TRUE(CompositionBuilder::build(timeline,MediaManifest{},"",missingPlan,&error)) << error;
    EXPECT_TRUE(missingPlan.gradeLuts.empty());
    EXPECT_FALSE(missingPlan.gradeLutErrors.at(asset->id).empty());
    LutLibrary::setDefaultRoot(directory / "replacement-root");
    timeline.tracks.clear();
    EXPECT_EQ(plan.gradeLuts.at(asset->id)->cubeText,identity);
    EXPECT_EQ(plan.clips.front().visualSnapshot.effects,clip.effects);
}

TEST_F(LutLibraryTest, CompositionPlanPinsDisabledEffectsAreIgnoredAndOtherLutsArePinned) {
    using namespace heisenberg;
    LutLibrary library(root());
    auto asset = library.importFile(write("source.cube",identity));
    ASSERT_NE(asset,nullptr);
    LutLibrary::setDefaultRoot(root());
    Timeline timeline;
    timeline.id = "timeline";
    Track track;
    track.id = "video";
    Clip clip;
    clip.id = "clip"; clip.mediaRef = "offline"; clip.durationFrames = 10;
    auto effect = JsonValue::parse(R"({"type":"video.colorGrade","enabled":false,"params":{"lut":{"id":"missing-disabled","intensity":{"value":1}}}})");
    clip.effects = JsonValue::array({effect});
    effect["enabled"] = true;
    effect["params"]["lut"]["id"] = "missing-zero";
    effect["params"]["lut"]["intensity"]["value"] = 0;
    clip.effects->push_back(effect);
    effect["params"]["lut"]["id"] = asset->id;
    effect["params"]["lut"]["intensity"]["track"] = JsonValue::parse(R"({"keyframes":[{"frame":0,"value":0},{"frame":9,"value":1}]})");
    clip.effects->push_back(effect);
    clip.effects->push_back(effect); // One resolution per distinct identity.
    track.clips.push_back(clip);
    timeline.tracks.push_back(track);
    CompositionPlan plan;
    std::string error;
    ASSERT_TRUE(CompositionBuilder::build(timeline,MediaManifest{},"",plan,&error)) << error;
    ASSERT_EQ(plan.gradeLuts.size(),1u);
    EXPECT_TRUE(plan.gradeLuts.contains(asset->id)); // Animated input resolves despite base intensity zero.
    ASSERT_EQ(plan.gradeLutErrors.size(),1u);
    EXPECT_TRUE(plan.gradeLutErrors.contains("missing-zero"));
    EXPECT_FALSE(plan.gradeLutErrors.contains("missing-disabled"));
}

TEST(LutDomainTest, RequiresExplicitKnownCreativeDomainWithoutGuessing) {
    LutDomain domain = LutDomain::Rec709;
    EXPECT_TRUE(parseLutDomain("acescct",domain));
    EXPECT_EQ(domain,LutDomain::ACEScct);
    EXPECT_EQ(lutDomainName(domain),"acescct");
    EXPECT_TRUE(parseLutDomain("rec709",domain));
    EXPECT_EQ(domain,LutDomain::Rec709);
    EXPECT_EQ(lutDomainName(domain),"rec709");
    for (const auto* name : {"", "auto", "linear", "ACEScct", "srgb", "rec1886"}) {
        EXPECT_FALSE(parseLutDomain(name,domain));
        EXPECT_EQ(domain,LutDomain::Rec709);
    }
}
} // namespace
