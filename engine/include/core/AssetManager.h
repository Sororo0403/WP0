#pragma once
#include <filesystem>

class AssetManager {
public:
    /// <summary>
    /// AssetRootを設定する
    /// </summary>
    static void SetAssetRoot(std::filesystem::path assetRoot);
    static std::filesystem::path GetAssetRoot();
    static void SetEngineResourceRoot(std::filesystem::path root);
    static void SetProjectAssetRoot(std::filesystem::path root);
    static void SetUserDataRoot(std::filesystem::path root);
    static std::filesystem::path GetEngineResourceRoot();
    static std::filesystem::path GetProjectAssetRoot();
    static std::filesystem::path GetUserDataRoot();
    // ビルド出力ディレクトリから起動した場合でも、親ディレクトリを
    // たどってエンジン・アプリの固定リソースを探す。
    static std::filesystem::path ResolvePath(const std::filesystem::path& relativePath);
    // asset:// URIまたはプロジェクトアセット相対パスを解決する。
    // 解決後のパスはProjectAssetRoot内に制限される。
    static std::filesystem::path ResolvePathStrict(const std::filesystem::path& relativePath);

private:
    /// <summary>
    /// onicalizeかを取得する
    /// </summary>
    static std::filesystem::path Canonicalize(const std::filesystem::path& path);
};
