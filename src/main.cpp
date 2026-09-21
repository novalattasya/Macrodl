#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
#include <matjson.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_DOWNLOADS = 10;

struct ReplayInfo {
    int64_t id = 0;
    std::string format;
    std::string url;
    std::string filename;
};

struct Outcome {
    bool ok = false;
    std::string message;
};

std::string g_inertiaVersion;
bool g_busy = false;

std::string extractVersion(std::string const& html) {
    for (char const* marker : {"&quot;version&quot;:&quot;", "&#34;version&#34;:&#34;", "\"version\":\""}) {
        size_t pos = 0;
        while ((pos = html.find(marker, pos)) != std::string::npos) {
            pos += std::strlen(marker);
            size_t end = pos;
            while (end < html.size() && std::isxdigit(static_cast<unsigned char>(html[end]))) end++;
            if (end - pos == 32) return html.substr(pos, 32);
        }
    }
    return "";
}

web::WebRequest baseRequest() {
    web::WebRequest req;
    req.userAgent("GeodeHyperbolusMacros/1.0");
    return req;
}

fs::path targetFolder() {
    auto custom = Mod::get()->getSettingValue<std::string>("save-folder");
    fs::path dir = custom.empty() ? (Mod::get()->getSaveDir() / "macros") : fs::path(custom);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

arc::Future<Outcome> lookupAndDownload(int levelId, fs::path dir) {
    std::string pageUrl = fmt::format("https://hyperbolus.net/level/{}/replays", levelId);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (g_inertiaVersion.empty()) {
            auto req = baseRequest();
            auto res = co_await req.get(pageUrl);
            if (!res.ok()) {
                co_return Outcome{false, fmt::format("Gagal membuka Hyperbolus (HTTP {}).", res.code())};
            }
            g_inertiaVersion = extractVersion(res.string().unwrapOr(""));
            if (g_inertiaVersion.empty()) {
                co_return Outcome{false, "Tidak bisa membaca versi halaman Hyperbolus (format situs berubah atau diblokir)."};
            }
        }

        auto req = baseRequest();
        req.header("Accept", "text/html, application/xhtml+xml");
        req.header("X-Requested-With", "XMLHttpRequest");
        req.header("X-Inertia", "true");
        req.header("X-Inertia-Version", g_inertiaVersion);
        auto res = co_await req.get(pageUrl);

        if (res.code() == 409) {
            g_inertiaVersion.clear();
            continue;
        }
        if (!res.ok()) {
            co_return Outcome{false, fmt::format("Gagal mengambil daftar macro (HTTP {}).", res.code())};
        }

        auto parsed = res.json();
        if (parsed.isErr()) {
            co_return Outcome{false, "Respons Hyperbolus tidak bisa dibaca (bukan JSON)."};
        }
        auto root = parsed.unwrap();

        std::vector<ReplayInfo> replays;
        auto arr = root["props"]["replays"]["data"].asArray();
        if (arr.isOk()) {
            for (auto item : arr.unwrap()) {
                auto files = item["files"].asArray();
                if (files.isErr() || files.unwrap().empty()) continue;
                auto file = files.unwrap()[0];

                ReplayInfo r;
                r.id = item["id"].asInt().unwrapOr(0);
                r.format = item["format"].asString().unwrapOr("");
                r.url = file["url"].asString().unwrapOr("");
                r.filename = file["filename"].asString().unwrapOr("");
                if (!r.url.empty()) replays.push_back(std::move(r));
            }
        }
        if (replays.empty()) {
            co_return Outcome{false, "Belum ada macro untuk level ini di Hyperbolus."};
        }

        int saved = 0;
        std::string lastName;
        std::string lastError;
        for (size_t i = 0; i < replays.size() && i < MAX_DOWNLOADS; ++i) {
            auto const& r = replays[i];
            auto dreq = baseRequest();
            auto dres = co_await dreq.get(r.url);
            if (!dres.ok()) {
                lastError = fmt::format("Download gagal (HTTP {}).", dres.code());
                continue;
            }

            std::string name = fs::path(r.filename).filename().string();
            if (name.empty()) {
                name = fmt::format("{}_{}.{}", levelId, r.id, r.format.empty() ? "macro" : r.format);
            }
            auto path = dir / name;
            if (fs::exists(path)) {
                auto p = fs::path(name);
                path = dir / fmt::format("{}_{}{}", p.stem().string(), r.id, p.extension().string());
            }

            auto written = file::writeBinary(path, dres.data());
            if (written.isErr()) {
                lastError = fmt::format("Gagal menyimpan file ke {}", path.string());
                continue;
            }
            saved++;
            lastName = path.filename().string();
        }

        if (saved == 0) {
            co_return Outcome{false, lastError.empty() ? "Tidak ada macro yang berhasil disimpan." : lastError};
        }
        if (saved == 1) {
            co_return Outcome{true, "Macro disimpan: " + lastName};
        }
        co_return Outcome{true, fmt::format("{} macro disimpan ke {}", saved, dir.string())};
    }

    co_return Outcome{false, "Versi halaman Hyperbolus tidak cocok. Coba lagi."};
}

}

class $modify(HypLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        auto spr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_downloadBtn_001.png", 0.9f, CircleBaseColor::Blue, CircleBaseSize::Medium
        );
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(HypLevelInfoLayer::onHyperbolus));
        btn->setID("hyperbolus-macro-button"_spr);

        if (auto menu = this->getChildByID("left-side-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto fallback = CCMenu::create();
            fallback->setPosition({40.f, 100.f});
            fallback->addChild(btn);
            this->addChild(fallback);
        }
        return true;
    }

    void onHyperbolus(CCObject*) {
        if (g_busy) return;

        int levelId = m_level->m_levelID.value();
        if (levelId <= 0) {
            FLAlertLayer::create("Hyperbolus", "Level ini belum diupload, jadi tidak punya level ID.", "OK")->show();
            return;
        }

        g_busy = true;
        Notification::create("Mencari macro di Hyperbolus...", NotificationIcon::Loading)->show();

        async::spawn(lookupAndDownload(levelId, targetFolder()), [](Outcome outcome) {
            g_busy = false;
            if (outcome.ok) {
                Notification::create(outcome.message, NotificationIcon::Success)->show();
            } else {
                FLAlertLayer::create("Hyperbolus", outcome.message, "OK")->show();
            }
        });
    }
};
