#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>
#include <matjson.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_PAGES = 5;
constexpr size_t ROWS_PER_PAGE = 4;

struct ReplayInfo {
    int64_t id = 0;
    int fps = 0;
    int64_t bytes = 0;
    int64_t downloads = 0;
    std::string format;
    std::string author;
    std::string url;
    std::string filename;
};

struct FetchResult {
    bool ok = false;
    std::string error;
    std::vector<ReplayInfo> replays;
};

struct SaveResult {
    bool ok = false;
    std::string error;
    int64_t bytes = 0;
};

struct LibraryEntry {
    int64_t replayId = 0;
    int64_t bytes = 0;
    int64_t time = 0;
    int levelId = 0;
    int fps = 0;
    std::string levelName;
    std::string author;
    std::string format;
    std::string path;
};

struct Config {
    fs::path dir;
    std::string filter;
    std::string naming;
    std::string existing;
    bool confirm = false;
};

std::string g_inertiaVersion;
bool g_busy = false;
bool g_libraryLoaded = false;
std::vector<LibraryEntry> g_library;

std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string toUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string asciiOnly(std::string s) {
    for (auto& c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u < 32 || u > 126) c = '?';
    }
    return s;
}

std::string shorten(std::string s, size_t maxLength) {
    s = asciiOnly(std::move(s));
    if (s.size() > maxLength) s = s.substr(0, maxLength - 3) + "...";
    return s;
}

std::string sanitizeName(std::string s) {
    for (auto& c : s) {
        auto u = static_cast<unsigned char>(c);
        if (u < 32 || std::strchr("<>:\"/\\|?*", c) != nullptr) c = '_';
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

std::string formatBytes(int64_t bytes) {
    if (bytes < 1024) return fmt::format("{} B", bytes);
    if (bytes < 1024 * 1024) return fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
    return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

int64_t parseInt(std::string const& s) {
    int64_t value = 0;
    std::from_chars(s.data(), s.data() + s.size(), value);
    return value;
}

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

std::string formatOf(ReplayInfo const& r) {
    std::string f = toLower(r.format);
    if (f.empty()) {
        auto ext = toLower(fs::path(r.filename).extension().string());
        if (ext.size() > 1) f = ext.substr(1);
    }
    return f;
}

bool matchesFilter(ReplayInfo const& r, std::string const& filter) {
    auto f = formatOf(r);
    if (filter == "GDR") return f == "gdr";
    if (filter == "GDR2") return f == "gdr2";
    if (filter == "GDR + GDR2") return f == "gdr" || f == "gdr2";
    return true;
}

std::string joinParts(std::vector<std::string> const& parts) {
    std::string out;
    for (auto const& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += " - ";
        out += p;
    }
    return out;
}

web::WebRequest baseRequest() {
    web::WebRequest req;
    req.userAgent("Macrodl/1.0");
    return req;
}

Config readConfig() {
    auto mod = Mod::get();
    Config cfg;
    auto custom = mod->getSettingValue<std::filesystem::path>("save-folder");
    cfg.dir = custom.empty() ? (mod->getSaveDir() / "macros") : custom;
    cfg.filter = mod->getSettingValue<std::string>("format-filter");
    cfg.naming = mod->getSettingValue<std::string>("file-naming");
    cfg.existing = mod->getSettingValue<std::string>("existing-file");
    cfg.confirm = mod->getSettingValue<bool>("always-confirm");
    return cfg;
}

fs::path libraryPath() {
    return Mod::get()->getSaveDir() / "library.tsv";
}

std::string cleanField(std::string s) {
    for (auto& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

std::vector<std::string> splitTabs(std::string const& line) {
    std::vector<std::string> out;
    std::string current;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

void loadLibrary() {
    if (g_libraryLoaded) return;
    g_libraryLoaded = true;
    g_library.clear();
    std::ifstream in(libraryPath(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto p = splitTabs(line);
        if (p.size() < 9) continue;
        LibraryEntry e;
        e.replayId = parseInt(p[0]);
        e.levelId = static_cast<int>(parseInt(p[1]));
        e.fps = static_cast<int>(parseInt(p[2]));
        e.bytes = parseInt(p[3]);
        e.time = parseInt(p[4]);
        e.levelName = p[5];
        e.author = p[6];
        e.format = p[7];
        e.path = p[8];
        if (!e.path.empty()) g_library.push_back(std::move(e));
    }
}

void saveLibrary() {
    std::ofstream out(libraryPath(), std::ios::binary | std::ios::trunc);
    for (auto const& e : g_library) {
        out << e.replayId << '\t' << e.levelId << '\t' << e.fps << '\t' << e.bytes << '\t' << e.time << '\t'
            << cleanField(e.levelName) << '\t' << cleanField(e.author) << '\t' << cleanField(e.format) << '\t'
            << cleanField(e.path) << '\n';
    }
}

void pruneLibrary() {
    loadLibrary();
    auto before = g_library.size();
    std::error_code ec;
    std::erase_if(g_library, [&](LibraryEntry const& e) {
        return !fs::exists(fs::path(e.path), ec);
    });
    if (g_library.size() != before) saveLibrary();
}

LibraryEntry const* findDownloaded(int64_t replayId) {
    loadLibrary();
    std::error_code ec;
    for (auto const& e : g_library) {
        if (e.replayId == replayId && fs::exists(fs::path(e.path), ec)) return &e;
    }
    return nullptr;
}

void addLibraryEntry(LibraryEntry entry) {
    loadLibrary();
    std::erase_if(g_library, [&](LibraryEntry const& e) {
        return e.replayId == entry.replayId || e.path == entry.path;
    });
    g_library.push_back(std::move(entry));
    saveLibrary();
}

void deleteLibraryEntry(std::string const& path) {
    loadLibrary();
    std::error_code ec;
    fs::remove(fs::path(path), ec);
    std::erase_if(g_library, [&](LibraryEntry const& e) {
        return e.path == path;
    });
    saveLibrary();
}

void deleteAllEntries() {
    loadLibrary();
    std::error_code ec;
    for (auto const& e : g_library) fs::remove(fs::path(e.path), ec);
    g_library.clear();
    saveLibrary();
}

std::optional<fs::path> resolveTarget(Config const& cfg, ReplayInfo const& r, int levelId, std::string const& levelName) {
    std::error_code ec;
    fs::create_directories(cfg.dir, ec);

    std::string original = sanitizeName(fs::path(r.filename).filename().string());
    if (original.empty()) {
        auto f = formatOf(r);
        original = fmt::format("{}_{}.{}", levelId, r.id, f.empty() ? "macro" : f);
    }

    std::string name = original;
    if (cfg.naming == "Level name - original") {
        auto levelPart = sanitizeName(levelName);
        if (!levelPart.empty()) name = levelPart + " - " + original;
    } else if (cfg.naming == "Level ID - original") {
        name = fmt::format("{} - {}", levelId, original);
    }

    auto path = cfg.dir / fs::path(name);
    if (!fs::exists(path, ec)) return path;
    if (cfg.existing == "Overwrite") return path;
    if (cfg.existing == "Skip") return std::nullopt;

    auto stem = path.stem().string();
    auto ext = path.extension().string();
    for (int i = 2; i < 1000; ++i) {
        auto candidate = cfg.dir / fs::path(fmt::format("{} ({}){}", stem, i, ext));
        if (!fs::exists(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

arc::Future<FetchResult> fetchReplays(int levelId) {
    std::string pageUrl = fmt::format("https://hyperbolus.net/level/{}/replays", levelId);
    FetchResult result;
    int64_t lastPage = 1;

    for (int64_t page = 1; page <= lastPage && page <= static_cast<int64_t>(MAX_PAGES); ++page) {
        std::string url = page == 1 ? pageUrl : fmt::format("{}?page={}", pageUrl, page);
        bool done = false;

        for (int attempt = 0; attempt < 2 && !done; ++attempt) {
            if (g_inertiaVersion.empty()) {
                auto versionReq = baseRequest();
                auto versionRes = co_await versionReq.get(pageUrl);
                if (!versionRes.ok()) {
                    result.error = fmt::format("Could not reach Hyperbolus (HTTP {}).", versionRes.code());
                    co_return result;
                }
                g_inertiaVersion = extractVersion(versionRes.string().unwrapOr(""));
                if (g_inertiaVersion.empty()) {
                    result.error = "Could not read the Hyperbolus page. The site may have changed or blocked the request.";
                    co_return result;
                }
            }

            auto req = baseRequest();
            req.header("Accept", "text/html, application/xhtml+xml");
            req.header("X-Requested-With", "XMLHttpRequest");
            req.header("X-Inertia", "true");
            req.header("X-Inertia-Version", g_inertiaVersion);
            auto res = co_await req.get(url);

            if (res.code() == 409) {
                g_inertiaVersion.clear();
                continue;
            }
            if (!res.ok()) {
                result.error = fmt::format("Could not load the macro list (HTTP {}).", res.code());
                co_return result;
            }

            auto parsed = res.json();
            if (parsed.isErr()) {
                result.error = "Hyperbolus returned an unreadable response.";
                co_return result;
            }
            auto root = parsed.unwrap();
            auto block = root["props"]["replays"];
            lastPage = block["last_page"].asInt().unwrapOr(1);

            auto arr = block["data"].asArray();
            if (arr.isOk()) {
                for (auto item : arr.unwrap()) {
                    auto files = item["files"].asArray();
                    if (files.isErr() || files.unwrap().empty()) continue;
                    auto file = files.unwrap()[0];

                    ReplayInfo r;
                    r.id = item["id"].asInt().unwrapOr(0);
                    r.fps = static_cast<int>(item["fps"].asInt().unwrapOr(0));
                    r.format = item["format"].asString().unwrapOr("");
                    r.author = item["author"]["name"].asString().unwrapOr("");
                    r.url = file["url"].asString().unwrapOr("");
                    r.filename = file["filename"].asString().unwrapOr("");
                    r.bytes = file["bytes"].asInt().unwrapOr(0);
                    r.downloads = file["downloads"].asInt().unwrapOr(0);
                    if (!r.url.empty()) result.replays.push_back(std::move(r));
                }
            }
            done = true;
        }

        if (!done) {
            result.error = "The Hyperbolus page version did not match. Please try again.";
            co_return result;
        }
    }

    result.ok = true;
    co_return result;
}

arc::Future<SaveResult> downloadFile(std::string url, fs::path path) {
    SaveResult result;
    auto req = baseRequest();
    auto res = co_await req.get(url);
    if (!res.ok()) {
        result.error = fmt::format("Download failed (HTTP {}).", res.code());
        co_return result;
    }
    auto written = file::writeBinary(path, res.data());
    if (written.isErr()) {
        result.error = "Could not write the file to " + path.string();
        co_return result;
    }
    result.ok = true;
    result.bytes = static_cast<int64_t>(res.data().size());
    co_return result;
}

class ListPopup : public Popup {
public:
    struct Row {
        std::string key;
        std::string title;
        std::string subtitle;
    };

    using ActionCallback = std::function<void(ListPopup*, std::string const&)>;
    using FooterButton = std::pair<std::string, std::function<void(ListPopup*)>>;

    static ListPopup* create(
        std::string title,
        std::vector<Row> rows,
        bool destructive,
        std::string emptyText,
        ActionCallback onAction,
        std::vector<FooterButton> footer
    ) {
        auto ret = new ListPopup();
        if (ret->init(std::move(title), std::move(rows), destructive, std::move(emptyText), std::move(onAction), std::move(footer))) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void close() {
        this->keyBackClicked();
    }

    void removeKey(std::string const& key) {
        std::erase_if(m_rows, [&](Row const& r) {
            return r.key == key;
        });
        this->rebuild();
    }

    void clearRows() {
        m_rows.clear();
        this->rebuild();
    }

protected:
    std::vector<Row> m_rows;
    std::string m_emptyText;
    bool m_destructive = false;
    size_t m_page = 0;
    ActionCallback m_onAction;
    std::vector<FooterButton> m_footer;
    std::vector<CCNode*> m_dynamic;
    CCLabelBMFont* m_pageLabel = nullptr;

    bool init(
        std::string title,
        std::vector<Row> rows,
        bool destructive,
        std::string emptyText,
        ActionCallback onAction,
        std::vector<FooterButton> footer
    ) {
        if (!Popup::init(340.f, footer.empty() ? 262.f : 300.f)) return false;

        m_rows = std::move(rows);
        m_destructive = destructive;
        m_emptyText = std::move(emptyText);
        m_onAction = std::move(onAction);
        m_footer = std::move(footer);

        this->setTitle(title.c_str());

        float pagerY = m_footer.empty() ? 26.f : 62.f;

        auto leftSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        leftSpr->setScale(0.7f);
        auto rightSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        rightSpr->setScale(0.7f);
        rightSpr->setFlipX(true);

        auto prev = CCMenuItemSpriteExtra::create(leftSpr, this, menu_selector(ListPopup::onPrev));
        auto next = CCMenuItemSpriteExtra::create(rightSpr, this, menu_selector(ListPopup::onNext));
        m_buttonMenu->addChildAtPosition(prev, Anchor::Bottom, ccp(-60.f, pagerY));
        m_buttonMenu->addChildAtPosition(next, Anchor::Bottom, ccp(60.f, pagerY));

        m_pageLabel = CCLabelBMFont::create("1/1", "goldFont.fnt");
        m_pageLabel->setScale(0.6f);
        m_mainLayer->addChildAtPosition(m_pageLabel, Anchor::Bottom, ccp(0.f, pagerY));

        for (size_t i = 0; i < m_footer.size(); ++i) {
            auto spr = ButtonSprite::create(m_footer[i].first.c_str());
            spr->setScale(0.75f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ListPopup::onFooter));
            btn->setTag(static_cast<int>(i));
            float x = (static_cast<float>(i) - (static_cast<float>(m_footer.size()) - 1.f) / 2.f) * 120.f;
            m_buttonMenu->addChildAtPosition(btn, Anchor::Bottom, ccp(x, 24.f));
        }

        this->rebuild();
        return true;
    }

    size_t pageCount() const {
        return std::max<size_t>(1, (m_rows.size() + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
    }

    void rebuild() {
        for (auto node : m_dynamic) node->removeFromParent();
        m_dynamic.clear();

        auto pages = this->pageCount();
        if (m_page >= pages) m_page = pages - 1;
        m_pageLabel->setString(fmt::format("{}/{}", m_page + 1, pages).c_str());

        if (m_rows.empty()) {
            auto label = CCLabelBMFont::create(m_emptyText.c_str(), "bigFont.fnt");
            label->limitLabelWidth(m_size.width - 40.f, 0.6f, 0.3f);
            m_mainLayer->addChildAtPosition(label, Anchor::Center, ccp(0.f, 10.f));
            m_dynamic.push_back(label);
            return;
        }

        size_t start = m_page * ROWS_PER_PAGE;
        for (size_t i = 0; i < ROWS_PER_PAGE && start + i < m_rows.size(); ++i) {
            auto const& row = m_rows[start + i];
            float y = -60.f - static_cast<float>(i) * 44.f;

            auto title = CCLabelBMFont::create(shorten(row.title, 60).c_str(), "bigFont.fnt");
            title->setAnchorPoint(ccp(0.f, 0.5f));
            title->limitLabelWidth(m_size.width - 110.f, 0.5f, 0.2f);
            m_mainLayer->addChildAtPosition(title, Anchor::TopLeft, ccp(20.f, y + 9.f));
            m_dynamic.push_back(title);

            auto subtitle = CCLabelBMFont::create(shorten(row.subtitle, 90).c_str(), "chatFont.fnt");
            subtitle->setAnchorPoint(ccp(0.f, 0.5f));
            subtitle->setColor(ccc3(170, 170, 170));
            subtitle->limitLabelWidth(m_size.width - 110.f, 0.7f, 0.3f);
            m_mainLayer->addChildAtPosition(subtitle, Anchor::TopLeft, ccp(20.f, y - 9.f));
            m_dynamic.push_back(subtitle);

            CCNode* icon = nullptr;
            if (m_destructive) {
                auto trash = CCSprite::createWithSpriteFrameName("GJ_trashBtn_001.png");
                trash->setScale(0.75f);
                icon = trash;
            } else {
                auto get = ButtonSprite::create("Get");
                get->setScale(0.7f);
                icon = get;
            }
            auto btn = CCMenuItemSpriteExtra::create(icon, this, menu_selector(ListPopup::onAction));
            btn->setTag(static_cast<int>(start + i));
            m_buttonMenu->addChildAtPosition(btn, Anchor::TopRight, ccp(-38.f, y));
            m_dynamic.push_back(btn);
        }
    }

    void onPrev(CCObject*) {
        auto pages = this->pageCount();
        m_page = (m_page + pages - 1) % pages;
        this->rebuild();
    }

    void onNext(CCObject*) {
        auto pages = this->pageCount();
        m_page = (m_page + 1) % pages;
        this->rebuild();
    }

    void onAction(CCObject* sender) {
        auto index = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (index >= m_rows.size() || !m_onAction) return;
        auto key = m_rows[index].key;
        auto callback = m_onAction;
        callback(this, key);
    }

    void onFooter(CCObject* sender) {
        auto index = static_cast<size_t>(static_cast<CCNode*>(sender)->getTag());
        if (index >= m_footer.size() || !m_footer[index].second) return;
        auto callback = m_footer[index].second;
        callback(this);
    }
};

void startDownload(ReplayInfo r, int levelId, std::string levelName, Config cfg) {
    auto target = resolveTarget(cfg, r, levelId, levelName);
    if (!target) {
        g_busy = false;
        Notification::create("A file with the same name already exists. Skipped.", NotificationIcon::Warning)->show();
        return;
    }

    g_busy = true;
    Notification::create("Downloading macro...", NotificationIcon::Loading)->show();

    auto path = *target;
    async::spawn(downloadFile(r.url, path), [r, levelId, levelName, path](SaveResult result) {
        g_busy = false;
        if (!result.ok) {
            FLAlertLayer::create("Macrodl", result.error, "OK")->show();
            return;
        }

        LibraryEntry entry;
        entry.replayId = r.id;
        entry.levelId = levelId;
        entry.fps = r.fps;
        entry.bytes = result.bytes;
        entry.time = static_cast<int64_t>(std::time(nullptr));
        entry.levelName = levelName;
        entry.author = r.author;
        entry.format = formatOf(r);
        entry.path = path.string();
        addLibraryEntry(std::move(entry));

        Notification::create("Saved: " + path.filename().string(), NotificationIcon::Success)->show();
    });
}

void beginDownload(ReplayInfo r, int levelId, std::string levelName, Config cfg) {
    auto existing = findDownloaded(r.id);
    if (existing == nullptr) {
        startDownload(std::move(r), levelId, std::move(levelName), std::move(cfg));
        return;
    }

    auto name = fs::path(existing->path).filename().string();
    createQuickPopup(
        "Already downloaded",
        fmt::format("This macro is already in your library as {}. Download it again?", name),
        "Cancel",
        "Download",
        [r, levelId, levelName, cfg](auto, bool btn2) {
            if (btn2) startDownload(r, levelId, levelName, cfg);
        }
    );
}

void showPicker(std::vector<ReplayInfo> replays, int levelId, std::string levelName, Config cfg) {
    std::vector<ListPopup::Row> rows;
    for (auto const& r : replays) {
        ListPopup::Row row;
        row.key = std::to_string(r.id);
        row.title = fmt::format("{} - {}", r.author.empty() ? "Unknown" : r.author, formatOf(r).empty() ? "?" : toUpper(formatOf(r)));
        std::vector<std::string> parts;
        if (r.fps > 0) parts.push_back(fmt::format("{} FPS", r.fps));
        if (r.bytes > 0) parts.push_back(formatBytes(r.bytes));
        if (r.downloads > 0) parts.push_back(fmt::format("{} downloads", r.downloads));
        parts.push_back(fs::path(r.filename).filename().string());
        row.subtitle = joinParts(parts);
        rows.push_back(std::move(row));
    }

    auto onAction = [replays, levelId, levelName, cfg](ListPopup* popup, std::string const& key) {
        popup->close();
        for (auto const& r : replays) {
            if (std::to_string(r.id) == key) {
                beginDownload(r, levelId, levelName, cfg);
                break;
            }
        }
    };

    auto popup = ListPopup::create("Choose a macro", std::move(rows), false, "No macros to show.", onAction, {});
    if (popup) popup->show();
}

void runLookup(int levelId, std::string levelName) {
    if (g_busy) {
        Notification::create("A request is already in progress.", NotificationIcon::Warning)->show();
        return;
    }

    g_busy = true;
    auto cfg = readConfig();
    Notification::create("Searching for macros...", NotificationIcon::Loading)->show();

    async::spawn(fetchReplays(levelId), [levelId, levelName, cfg](FetchResult result) {
        g_busy = false;

        if (!result.ok) {
            FLAlertLayer::create("Macrodl", result.error, "OK")->show();
            return;
        }
        if (result.replays.empty()) {
            FLAlertLayer::create("Macrodl", "No macros were found for this level on Hyperbolus.", "OK")->show();
            return;
        }

        std::vector<ReplayInfo> matches;
        for (auto const& r : result.replays) {
            if (matchesFilter(r, cfg.filter)) matches.push_back(r);
        }
        if (matches.empty()) {
            FLAlertLayer::create(
                "Macrodl",
                fmt::format(
                    "{} macro(s) found, but none match the format filter ({}). You can change the filter in the Macrodl settings.",
                    result.replays.size(),
                    cfg.filter
                ),
                "OK"
            )->show();
            return;
        }

        std::stable_sort(matches.begin(), matches.end(), [](ReplayInfo const& a, ReplayInfo const& b) {
            return a.downloads > b.downloads;
        });

        if (matches.size() == 1 && !cfg.confirm) {
            beginDownload(matches[0], levelId, levelName, cfg);
            return;
        }
        showPicker(std::move(matches), levelId, levelName, cfg);
    });
}

void showLibrary() {
    pruneLibrary();

    std::vector<ListPopup::Row> rows;
    for (auto it = g_library.rbegin(); it != g_library.rend(); ++it) {
        ListPopup::Row row;
        row.key = it->path;
        row.title = fs::path(it->path).filename().string();
        std::string levelPart = it->levelName.empty() ? fmt::format("Level {}", it->levelId) : it->levelName;
        std::vector<std::string> parts;
        parts.push_back(levelPart);
        parts.push_back(it->author);
        parts.push_back(toUpper(it->format));
        parts.push_back(formatBytes(it->bytes));
        row.subtitle = joinParts(parts);
        rows.push_back(std::move(row));
    }

    auto onAction = [](ListPopup* popup, std::string const& key) {
        auto name = fs::path(key).filename().string();
        createQuickPopup(
            "Delete macro",
            fmt::format("Delete {} from your device?", name),
            "Cancel",
            "Delete",
            [ref = Ref<ListPopup>(popup), key](auto, bool btn2) {
                if (!btn2) return;
                deleteLibraryEntry(key);
                ref->removeKey(key);
            }
        );
    };

    std::vector<ListPopup::FooterButton> footer;
    footer.emplace_back("Settings", [](ListPopup*) {
        openSettingsPopup(Mod::get(), false);
    });
    footer.emplace_back("Delete all", [](ListPopup* popup) {
        if (g_library.empty()) {
            Notification::create("There are no macros to delete.", NotificationIcon::Warning)->show();
            return;
        }
        createQuickPopup(
            "Delete all macros",
            fmt::format("Delete all {} downloaded macros from your device?", g_library.size()),
            "Cancel",
            "Delete",
            [ref = Ref<ListPopup>(popup)](auto, bool btn2) {
                if (!btn2) return;
                deleteAllEntries();
                ref->clearRows();
            }
        );
    });

    auto popup = ListPopup::create("Downloaded macros", std::move(rows), true, "No macros downloaded yet.", onAction, std::move(footer));
    if (popup) popup->show();
}

}

class $modify(MacrodlLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        auto downloadSpr = CircleButtonSprite::createWithSpriteFrameName(
            "GJ_downloadBtn_001.png", 0.9f, CircleBaseColor::Blue, CircleBaseSize::Medium
        );
        auto downloadBtn = CCMenuItemSpriteExtra::create(downloadSpr, this, menu_selector(MacrodlLevelInfoLayer::onMacrodlDownload));
        downloadBtn->setID("macrodl-download-button"_spr);

        auto librarySpr = CircleButtonSprite::createWithSpriteFrameName(
            "folderIcon_001.png", 0.9f, CircleBaseColor::Green, CircleBaseSize::Medium
        );
        auto libraryBtn = CCMenuItemSpriteExtra::create(librarySpr, this, menu_selector(MacrodlLevelInfoLayer::onMacrodlLibrary));
        libraryBtn->setID("macrodl-library-button"_spr);

        if (auto menu = this->getChildByID("left-side-menu")) {
            menu->addChild(downloadBtn);
            menu->addChild(libraryBtn);
            menu->updateLayout();
        } else {
            auto fallback = CCMenu::create();
            fallback->setPosition({40.f, 100.f});
            fallback->addChild(downloadBtn);
            fallback->addChild(libraryBtn);
            fallback->alignItemsVerticallyWithPadding(6.f);
            this->addChild(fallback);
        }
        return true;
    }

    void onMacrodlDownload(CCObject*) {
        int levelId = m_level->m_levelID.value();
        if (levelId <= 0) {
            FLAlertLayer::create("Macrodl", "This level has not been uploaded, so it has no level ID.", "OK")->show();
            return;
        }
        std::string levelName = m_level->m_levelName.c_str();
        runLookup(levelId, levelName);
    }

    void onMacrodlLibrary(CCObject*) {
        showLibrary();
    }
};
