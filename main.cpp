#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <array>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace geode::prelude;

constexpr int MAX_W = 20;

// ============================================================
//  Estado global
// ============================================================
struct Input {
    int frame;
    bool down;
    int button;
    bool p1;
};

struct State {
    // contadores del 1 al 20 (solo se muestran en reproduccion)
    std::array<int, MAX_W + 1> counts{};
    std::array<CCLabelBMFont*, MAX_W + 1> labels{};
    CCLabelBMFont* reactionLabel = nullptr;

    // ventana del aro
    bool inWindow = false;
    int entryFrame = 0;
    int clickFrame = -1;

    // macro (modo practica)
    std::vector<Input> macro;
    bool recording = false;
    bool playing = false;
    size_t playIdx = 0;
    bool injecting = false;

    // opciones del menu
    bool soundOn = true;
    bool circlesOn = true;
} S;

// ============================================================
//  Utilidades
// ============================================================

// Compila tanto si m_touchingRings es un vector como si es un CCArray*
template <class T>
static bool hasItems(T const& r) {
    if constexpr (requires { r.size(); }) return r.size() > 0;
    else return r && r->count() > 0;
}

// Convierte un valor del juego (int o SeedValue) a int
template <class T>
static int asInt(T const& v) {
    if constexpr (requires { v.value(); }) return static_cast<int>(v.value());
    else return static_cast<int>(v);
}

// Color por margen: 1 = rojo ... 20 = violeta
static ccColor3B colorFor(int w) {
    float h = (w - 1) / float(MAX_W) * 270.f;
    float c = 1.f, x = c * (1 - std::fabs(std::fmod(h / 60.f, 2.f) - 1));
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else { r = x; b = c; }
    return { (GLubyte)(r * 255), (GLubyte)(g * 255), (GLubyte)(b * 255) };
}

static float msPerFrame() {
    return 1000.f / float(Mod::get()->getSettingValue<int64_t>("tps"));
}

// Sonido propio: geode/config/tuusuario.framemargin/sounds/frame_N.(ogg|mp3|wav)
static std::filesystem::path soundFor(int w) {
    auto dir = Mod::get()->getConfigDir() / "sounds";
    for (auto ext : {".ogg", ".mp3", ".wav"}) {
        auto f = dir / fmt::format("frame_{}{}", w, ext);
        std::error_code ec;
        if (std::filesystem::exists(f, ec)) return f;
    }
    return {};
}

// Archivo de macro por nivel
static std::filesystem::path macroPath() {
    int id = 0;
    if (auto pl = PlayLayer::get()) {
        if (pl->m_level) id = asInt(pl->m_level->m_levelID);
    }
    auto dir = Mod::get()->getSaveDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / fmt::format("level_{}.txt", id);
}

$execute {
    log::info("[FrameMargin] v1.4.0 cargado");
}

// ============================================================
//  HUD: el contador SOLO se ve durante la reproduccion
// ============================================================
static void updateHud() {
    bool vis = S.playing;
    for (int w = 1; w <= MAX_W; ++w) {
        if (S.labels[w]) S.labels[w]->setVisible(vis);
    }
    if (S.reactionLabel) S.reactionLabel->setVisible(vis);
}

static void refreshCounts() {
    for (int w = 1; w <= MAX_W; ++w) {
        if (S.labels[w]) {
            S.labels[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
        }
    }
    if (S.reactionLabel) {
        S.reactionLabel->setString("React: -- ms");
        S.reactionLabel->setColor({255, 255, 255});
    }
}

static std::string statusText() {
    if (S.recording) return fmt::format("Grabando: {}", S.macro.size());
    if (S.playing) return "Reproduciendo";
    return "Inactivo";
}

// ============================================================
//  Acciones del menu
// ============================================================
static void fmRecord() {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_isPracticeMode) {
        FLAlertLayer::create("Frame Margin", "Activa el modo practica primero.", "OK")->show();
        return;
    }
    S.macro.clear();
    S.recording = true;
    S.playing = false;
    updateHud();
    Notification::create("Grabando... reinicia y juga", NotificationIcon::Success)->show();
}

static void fmStop() {
    S.recording = false;
    S.playing = false;
    updateHud();
    Notification::create(fmt::format("{} inputs guardados", S.macro.size()), NotificationIcon::Info)->show();
}

static void fmPlay() {
    if (S.macro.empty()) {
        Notification::create("No hay replay cargada", NotificationIcon::Warning)->show();
        return;
    }
    S.recording = false;
    S.playing = true;
    S.playIdx = 0;
    S.counts.fill(0);
    S.inWindow = false;
    S.clickFrame = -1;
    refreshCounts();
    updateHud();
    Notification::create("Reproduciendo: reinicia el nivel", NotificationIcon::Success)->show();
}

static void fmReset() {
    S.counts.fill(0);
    refreshCounts();
    Notification::create("Contadores en 0", NotificationIcon::Info)->show();
}

static void fmSave() {
    if (S.macro.empty()) {
        Notification::create("No hay nada para guardar", NotificationIcon::Warning)->show();
        return;
    }
    std::ofstream f(macroPath());
    if (!f) {
        Notification::create("No se pudo guardar", NotificationIcon::Error)->show();
        return;
    }
    for (auto const& in : S.macro) {
        f << in.frame << ' ' << (in.down ? 1 : 0) << ' ' << in.button << ' ' << (in.p1 ? 1 : 0) << '\n';
    }
    Notification::create(fmt::format("Guardada ({} inputs)", S.macro.size()), NotificationIcon::Success)->show();
}

static void fmLoad() {
    std::ifstream f(macroPath());
    if (!f) {
        Notification::create("No hay replay guardada para este nivel", NotificationIcon::Warning)->show();
        return;
    }
    std::vector<Input> loaded;
    int a, b, c, d;
    while (f >> a >> b >> c >> d) {
        loaded.push_back({a, b != 0, c, d != 0});
    }
    if (loaded.empty()) {
        Notification::create("Archivo vacio", NotificationIcon::Warning)->show();
        return;
    }
    S.macro = std::move(loaded);
    S.recording = false;
    S.playing = false;
    updateHud();
    Notification::create(fmt::format("Cargada ({} inputs)", S.macro.size()), NotificationIcon::Success)->show();
}

// ============================================================
//  PlayLayer: crea el HUD (oculto hasta que des Reproducir)
// ============================================================
class $modify(FMPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        log::info("[FrameMargin] PlayLayer::setupHasCompleted -> creando HUD");

        S.counts.fill(0);
        S.inWindow = false;

        auto winSize = CCDirector::get()->getWinSize();
        float y = winSize.height - 14.f;
        for (int w = 1; w <= MAX_W; ++w) {
            auto lbl = CCLabelBMFont::create(fmt::format("{}: 0", w).c_str(), "bigFont.fnt");
            lbl->setScale(0.32f);
            lbl->setAnchorPoint({0, 0.5f});
            lbl->setPosition({8.f, y});
            lbl->setColor(colorFor(w));
            lbl->setZOrder(1000);
            this->addChild(lbl);
            S.labels[w] = lbl;
            y -= 11.5f;
        }
        S.reactionLabel = CCLabelBMFont::create("React: -- ms", "bigFont.fnt");
        S.reactionLabel->setScale(0.36f);
        S.reactionLabel->setAnchorPoint({0, 0.5f});
        S.reactionLabel->setPosition({8.f, y - 6.f});
        S.reactionLabel->setZOrder(1000);
        this->addChild(S.reactionLabel);

        updateHud(); // oculto salvo que haya reproduccion activa

        Notification::create("Frame Margin v1.4.0 activo", NotificationIcon::Success)->show();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        S.inWindow = false;
        S.clickFrame = -1;
        S.playIdx = 0;
        if (S.recording) S.macro.clear(); // grabar desde el respawn
    }

    void onQuit() {
        S.labels.fill(nullptr);
        S.reactionLabel = nullptr;
        S.inWindow = false;
        S.recording = false;
        S.playing = false;
        PlayLayer::onQuit();
    }
};

// ============================================================
//  Logica por frame
// ============================================================
class $modify(FMBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
        if (!PlayLayer::get() || !m_player1) return;

        int frame = m_gameState.m_currentProgress;

        // reproducir macro
        if (S.playing) {
            while (S.playIdx < S.macro.size() && S.macro[S.playIdx].frame <= frame) {
                auto const& in = S.macro[S.playIdx];
                S.injecting = true;
                this->handleButton(in.down, in.button, in.p1);
                S.injecting = false;
                S.playIdx++;
            }
        }

        // el margen SOLO se mide durante la reproduccion
        if (!S.playing) {
            S.inWindow = false;
            return;
        }

        bool touching = hasItems(m_player1->m_touchingRings);

        if (touching && !S.inWindow) {
            S.inWindow = true;
            S.entryFrame = frame;
            S.clickFrame = -1;
        } else if (!touching && S.inWindow) {
            S.inWindow = false;
            int w = frame - S.entryFrame;
            if (S.clickFrame >= 0 && w >= 1 && w <= MAX_W) {
                onMarginFound(w, (S.clickFrame - S.entryFrame) * msPerFrame());
            }
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!PlayLayer::get()) return;

        int frame = m_gameState.m_currentProgress;

        // grabar solo los clicks reales, no los inyectados
        if (S.recording && !S.injecting) {
            S.macro.push_back({frame, down, button, isPlayer1});
        }
        // medir reaccion (clicks reales o reproducidos)
        if (down && S.inWindow && S.clickFrame < 0) {
            S.clickFrame = frame;
        }
    }

    void onMarginFound(int w, float reactionMs) {
        S.counts[w]++;
        if (S.labels[w]) {
            S.labels[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
        }
        if (S.reactionLabel) {
            S.reactionLabel->setString(fmt::format("React: {:.1f} ms ({}f)", reactionMs, w).c_str());
            S.reactionLabel->setColor(colorFor(w));
        }

        // Sonido: menos frames = mas fuerte
        if (S.soundOn) {
            float vol = std::clamp(1.f - (w - 1) * 0.045f, 0.1f, 1.f);
            vol *= float(Mod::get()->getSettingValue<double>("volume"));
            float speed = 1.5f - (w - 1) * 0.04f;
            auto custom = soundFor(w);
            if (!custom.empty()) {
                FMODAudioEngine::sharedEngine()->playEffect(custom.string(), 1.f, 0.f, vol);
            } else {
                FMODAudioEngine::sharedEngine()->playEffect("playSound_01.ogg", speed, 0.f, vol);
            }
        }

        // Circulito de color donde ocurrio el frame
        if (S.circlesOn) {
            auto dot = CCDrawNode::create();
            auto c = colorFor(w);
            dot->drawDot({0, 0}, 6.f, {c.r / 255.f, c.g / 255.f, c.b / 255.f, 0.9f});
            dot->setPosition(m_player1->getPosition());
            dot->setZOrder(100);
            m_objectLayer->addChild(dot);
        }
    }
};

// ============================================================
//  Menu interactivo en la pantalla de pausa (arriba a la izquierda)
// ============================================================
class $modify(FMPause, PauseLayer) {
    struct Fields {
        CCLabelBMFont* status = nullptr;
        CCLabelBMFont* info = nullptr;
    };

    void refreshFM() {
        if (m_fields->status) m_fields->status->setString(statusText().c_str());
        if (m_fields->info) {
            m_fields->info->setString(fmt::format("Replay: {} inputs", S.macro.size()).c_str());
        }
    }

    void customSetup() {
        PauseLayer::customSetup();
        log::info("[FrameMargin] PauseLayer::customSetup -> creando menu de pausa");

        auto winSize = CCDirector::get()->getWinSize();
        constexpr float W = 90.f;
        constexpr float H = 182.f;

        // contenedor del panel (esquina superior izquierda, debajo de los botones de GD)
        auto panel = CCNode::create();
        panel->setID("fm-panel");
        panel->setContentSize({W, H});
        panel->setAnchorPoint({0, 0});
        panel->setPosition({4.f, winSize.height - 96.f - H});

        // fondo
        if (auto bg = CCScale9Sprite::create("square02_001.png")) {
            bg->setContentSize({W, H});
            bg->setAnchorPoint({0, 0});
            bg->setPosition({0, 0});
            bg->setOpacity(130);
            panel->addChild(bg, -1);
        }

        // titulo y estado
        auto title = CCLabelBMFont::create("FRAME MARGIN", "goldFont.fnt");
        title->setScale(0.38f);
        title->setPosition({W / 2, H - 9.f});
        panel->addChild(title);

        m_fields->status = CCLabelBMFont::create(statusText().c_str(), "bigFont.fnt");
        m_fields->status->setScale(0.3f);
        m_fields->status->setPosition({W / 2, H - 20.f});
        panel->addChild(m_fields->status);

        m_fields->info = CCLabelBMFont::create(
            fmt::format("Replay: {} inputs", S.macro.size()).c_str(), "bigFont.fnt");
        m_fields->info->setScale(0.26f);
        m_fields->info->setPosition({W / 2, H - 30.f});
        panel->addChild(m_fields->info);

        // menu con botones y toggles
        auto menu = CCMenu::create();
        menu->setContentSize({W, H});
        menu->setAnchorPoint({0, 0});
        menu->setPosition({0, 0});
        menu->setTouchPriority(-600);

        float y = H - 46.f;
        auto addBtn = [&](char const* txt, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(txt, 80, true, "bigFont.fnt", "GJ_button_04.png", 18.f, 0.4f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition({W / 2, y});
            menu->addChild(btn);
            y -= 19.f;
        };
        addBtn("Grabar", menu_selector(FMPause::onFMRecord));
        addBtn("Parar", menu_selector(FMPause::onFMStop));
        addBtn("Reproducir", menu_selector(FMPause::onFMPlay));
        addBtn("Reset", menu_selector(FMPause::onFMReset));
        addBtn("Guardar", menu_selector(FMPause::onFMSave));
        addBtn("Cargar", menu_selector(FMPause::onFMLoad));

        // toggles
        auto addToggle = [&](char const* txt, bool on, SEL_MenuHandler cb, float ty) {
            auto t = CCMenuItemToggler::createWithStandardSprites(this, cb, 0.4f);
            t->toggle(on);
            t->setPosition({14.f, ty});
            menu->addChild(t);
            auto lbl = CCLabelBMFont::create(txt, "bigFont.fnt");
            lbl->setScale(0.3f);
            lbl->setAnchorPoint({0, 0.5f});
            lbl->setPosition({26.f, ty});
            panel->addChild(lbl);
        };
        addToggle("Sonido", S.soundOn, menu_selector(FMPause::onFMToggleSound), 25.f);
        addToggle("Circulos", S.circlesOn, menu_selector(FMPause::onFMToggleCircles), 10.f);

        panel->addChild(menu);
        this->addChild(panel, 1000);
    }

    void onFMRecord(CCObject*) { fmRecord(); refreshFM(); }
    void onFMStop(CCObject*) { fmStop(); refreshFM(); }
    void onFMPlay(CCObject*) { fmPlay(); refreshFM(); }
    void onFMReset(CCObject*) { fmReset(); refreshFM(); }
    void onFMSave(CCObject*) { fmSave(); refreshFM(); }
    void onFMLoad(CCObject*) { fmLoad(); refreshFM(); }

    void onFMToggleSound(CCObject* sender) {
        S.soundOn = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
    }
    void onFMToggleCircles(CCObject* sender) {
        S.circlesOn = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
    }
};
