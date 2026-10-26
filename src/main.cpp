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
    // contadores del 1 al 20 (HUD solo visible durante la reproduccion)
    std::array<int, MAX_W + 1> counts{};
    std::array<CCLabelBMFont*, MAX_W + 1> labels{};
    CCLabelBMFont* reactionLabel = nullptr;
    std::string lastReact = "React: -- ms";

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
    bool syncPending = false;   // al respawnear, saltar eventos anteriores al frame actual
    int injected = 0;           // clicks inyectados (diagnostico)
    int measured = 0;           // margenes medidos (diagnostico)

    // opciones
    bool soundOn = true;
    bool circlesOn = true;
    bool autoRestart = true;    // Reproducir reinicia el nivel solo

    // el HUD se oculta al pausar y se restaura al volver al juego
    bool hudRestore = false;
} S;

// ============================================================
//  Utilidades
// ============================================================
template <class T>
static bool hasItems(T const& r) {
    if constexpr (requires { r.size(); }) return r.size() > 0;
    else return r && r->count() > 0;
}

template <class T>
static int asInt(T& v) {
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
    log::info("[FrameMargin] v1.5.0 cargado");
}

static std::string statusText() {
    if (S.recording) return fmt::format("Grabando ({} inputs)", S.macro.size());
    if (S.playing) return "Reproduciendo";
    return "Inactivo";
}

// ============================================================
//  HUD: el contador SOLO se ve durante la reproduccion
// ============================================================
static void setHudVisible(bool vis) {
    for (int w = 1; w <= MAX_W; ++w) {
        if (S.labels[w]) S.labels[w]->setVisible(vis);
    }
    if (S.reactionLabel) S.reactionLabel->setVisible(vis);
}

static void updateHud() {
    setHudVisible(S.playing);
}

static void refreshPopup(); // definida despues del popup

static void refreshCounts() {
    S.lastReact = "React: -- ms";
    for (int w = 1; w <= MAX_W; ++w) {
        if (S.labels[w]) {
            S.labels[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
        }
    }
    if (S.reactionLabel) {
        S.reactionLabel->setString(S.lastReact.c_str());
        S.reactionLabel->setColor({255, 255, 255});
    }
}

// ============================================================
//  Acciones
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
    refreshPopup();
    Notification::create("Grabando... reinicia y juga", NotificationIcon::Success)->show();
}

static void fmStop() {
    S.recording = false;
    S.playing = false;
    updateHud();
    refreshPopup();
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
    S.syncPending = true;
    S.counts.fill(0);
    S.injected = 0;
    S.measured = 0;
    S.inWindow = false;
    S.clickFrame = -1;
    refreshCounts();
    updateHud();
    refreshPopup();
    Notification::create("Reproduciendo replay", NotificationIcon::Success)->show();
}

static void fmReset() {
    S.counts.fill(0);
    S.measured = 0;
    refreshCounts();
    refreshPopup();
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
    refreshPopup();
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

        updateHud();

        Notification::create("Frame Margin v1.5.0 activo", NotificationIcon::Success)->show();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        S.inWindow = false;
        S.clickFrame = -1;
        S.playIdx = 0;
        S.syncPending = true;
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
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!PlayLayer::get() || !m_player1) return;

        // volvimos del menu de pausa: restaurar el HUD si hay reproduccion
        if (S.hudRestore) {
            S.hudRestore = false;
            updateHud();
        }

        int frame = m_gameState.m_currentProgress;

        // reproducir macro
        if (S.playing) {
            // al empezar un intento, saltar eventos anteriores al frame de respawn
            if (S.syncPending) {
                while (S.playIdx < S.macro.size() && S.macro[S.playIdx].frame < frame) {
                    S.playIdx++;
                }
                S.syncPending = false;
            }
            while (S.playIdx < S.macro.size() && S.macro[S.playIdx].frame <= frame) {
                auto const& in = S.macro[S.playIdx];
                S.injecting = true;
                this->handleButton(in.down, in.button, in.p1);
                S.injecting = false;
                S.playIdx++;
                S.injected++;
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
        S.measured++;
        S.lastReact = fmt::format("React: {:.1f} ms ({}f)", reactionMs, w);
        if (S.labels[w]) {
            S.labels[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
        }
        if (S.reactionLabel) {
            S.reactionLabel->setString(S.lastReact.c_str());
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
//  Ventana interactiva (centro de la pantalla)
// ============================================================
class FMPopup;
static FMPopup* g_popup = nullptr;

class FMPopup : public CCLayer {
public:
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_react = nullptr;
    std::array<CCLabelBMFont*, MAX_W + 1> m_counts{};

    static FMPopup* create() {
        auto ret = new FMPopup();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    ~FMPopup() {
        if (g_popup == this) g_popup = nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        g_popup = this;

        auto winSize = CCDirector::get()->getWinSize();
        constexpr float W = 340.f;
        constexpr float H = 236.f;
        float cx = winSize.width / 2;
        float cy = winSize.height / 2;

        // fondo oscuro
        auto dim = CCLayerColor::create({0, 0, 0, 150});
        this->addChild(dim, 0);

        // bloquea los toques fuera de la ventana
        auto blockMenu = CCMenu::create();
        blockMenu->setPosition({0, 0});
        blockMenu->setTouchPriority(-700);
        auto blocker = CCScale9Sprite::create("square02_001.png");
        blocker->setContentSize(winSize);
        blocker->setOpacity(0);
        auto blockBtn = CCMenuItemSpriteExtra::create(blocker, this, menu_selector(FMPopup::onNoop));
        blockBtn->setPosition({cx, cy});
        blockMenu->addChild(blockBtn);
        this->addChild(blockMenu, 1);

        // panel
        auto panel = CCNode::create();
        panel->setContentSize({W, H});
        panel->setAnchorPoint({0.5f, 0.5f});
        panel->ignoreAnchorPointForPosition(false);
        panel->setPosition({cx, cy});
        this->addChild(panel, 2);

        if (auto bg = CCScale9Sprite::create("GJ_square01.png")) {
            bg->setContentSize({W, H});
            bg->setAnchorPoint({0, 0});
            bg->setPosition({0, 0});
            panel->addChild(bg, -1);
        }

        auto addLabel = [&](char const* txt, char const* font, float scale, float x, float y, bool center) {
            auto l = CCLabelBMFont::create(txt, font);
            l->setScale(scale);
            if (!center) l->setAnchorPoint({0, 0.5f});
            l->setPosition({x, y});
            panel->addChild(l);
            return l;
        };

        addLabel("Frame Margin", "goldFont.fnt", 0.8f, W / 2, H - 16.f, true);
        m_status = addLabel("", "bigFont.fnt", 0.34f, W / 2, H - 36.f, true);
        m_info = addLabel("", "bigFont.fnt", 0.26f, W / 2, H - 49.f, true);

        addLabel("REPLAY", "goldFont.fnt", 0.5f, 88.f, H - 66.f, true);
        addLabel("OPCIONES", "goldFont.fnt", 0.5f, 88.f, H - 160.f, true);
        addLabel("MARGENES", "goldFont.fnt", 0.5f, 262.f, H - 66.f, true);

        // menu de botones
        auto menu = CCMenu::create();
        menu->setContentSize({W, H});
        menu->setAnchorPoint({0, 0});
        menu->setPosition({0, 0});
        menu->setTouchPriority(-710);
        panel->addChild(menu, 5);

        // boton cerrar
        auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, this, menu_selector(FMPopup::onClose));
        closeBtn->setPosition({10.f, H - 10.f});
        menu->addChild(closeBtn);

        auto addBtn = [&](char const* txt, char const* tex, float x, float y, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(txt, 72, true, "bigFont.fnt", tex, 22.f, 0.45f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            btn->setPosition({x, y});
            menu->addChild(btn);
        };
        addBtn("Grabar", "GJ_button_01.png", 50.f, H - 88.f, menu_selector(FMPopup::onRecord));
        addBtn("Parar", "GJ_button_04.png", 126.f, H - 88.f, menu_selector(FMPopup::onStop));
        addBtn("Reproducir", "GJ_button_01.png", 50.f, H - 114.f, menu_selector(FMPopup::onPlay));
        addBtn("Reset", "GJ_button_04.png", 126.f, H - 114.f, menu_selector(FMPopup::onReset));
        addBtn("Guardar", "GJ_button_01.png", 50.f, H - 140.f, menu_selector(FMPopup::onSave));
        addBtn("Cargar", "GJ_button_01.png", 126.f, H - 140.f, menu_selector(FMPopup::onLoad));

        // interruptores
        auto addToggle = [&](char const* txt, bool on, SEL_MenuHandler cb, float x, float y) {
            auto t = CCMenuItemToggler::createWithStandardSprites(this, cb, 0.55f);
            t->toggle(on);
            t->setPosition({x, y});
            menu->addChild(t);
            addLabel(txt, "bigFont.fnt", 0.34f, x + 16.f, y, false);
        };
        addToggle("Sonido", S.soundOn, menu_selector(FMPopup::onToggleSound), 20.f, H - 180.f);
        addToggle("Circulos", S.circlesOn, menu_selector(FMPopup::onToggleCircles), 20.f, H - 200.f);
        addToggle("Auto-reiniciar", S.autoRestart, menu_selector(FMPopup::onToggleAuto), 20.f, H - 220.f);

        // contadores 1..20 en dos columnas
        for (int w = 1; w <= MAX_W; ++w) {
            int i = (w - 1) % 10;
            float x = (w <= 10) ? 200.f : 268.f;
            float y = H - 84.f - i * 13.f;
            auto l = addLabel("", "bigFont.fnt", 0.34f, x, y, false);
            l->setColor(colorFor(w));
            m_counts[w] = l;
        }
        m_react = addLabel("", "bigFont.fnt", 0.3f, 262.f, 20.f, true);

        refresh();
        return true;
    }

    void refresh() {
        if (m_status) m_status->setString(fmt::format("Estado: {}", statusText()).c_str());
        if (m_info) {
            m_info->setString(fmt::format("Replay: {} inputs | Inyectados: {} | Medidos: {}",
                S.macro.size(), S.injected, S.measured).c_str());
        }
        for (int w = 1; w <= MAX_W; ++w) {
            if (m_counts[w]) m_counts[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
        }
        if (m_react) m_react->setString(S.lastReact.c_str());
    }

    void onNoop(CCObject*) {}

    void onClose(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

    void onRecord(CCObject*) { fmRecord(); refresh(); }
    void onStop(CCObject*) { fmStop(); refresh(); }
    void onReset(CCObject*) { fmReset(); refresh(); }
    void onSave(CCObject*) { fmSave(); refresh(); }
    void onLoad(CCObject*) { fmLoad(); refresh(); }

    void onPlay(CCObject*) {
        fmPlay();
        refresh();
        // reiniciar el nivel automaticamente para que arranque la replay
        if (S.playing && S.autoRestart) {
            if (auto pause = typeinfo_cast<PauseLayer*>(this->getParent())) {
                pause->onRestart(nullptr);
            }
        }
    }

    void onToggleSound(CCObject* sender) {
        S.soundOn = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
    }
    void onToggleCircles(CCObject* sender) {
        S.circlesOn = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
    }
    void onToggleAuto(CCObject* sender) {
        S.autoRestart = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
    }
};

static void refreshPopup() {
    if (g_popup) g_popup->refresh();
}

// ============================================================
//  Pantalla de pausa: boton "FM" arriba a la izquierda
// ============================================================
class $modify(FMPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        log::info("[FrameMargin] PauseLayer::customSetup -> boton FM");

        // ocultar los contadores mientras estamos en pausa (no pisar los datos de GD)
        setHudVisible(false);
        S.hudRestore = true;

        auto winSize = CCDirector::get()->getWinSize();

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        menu->setTouchPriority(-600);

        auto spr = ButtonSprite::create("FM", 30, true, "goldFont.fnt", "GJ_button_01.png", 22.f, 0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FMPause::onFMOpen));
        btn->setPosition({104.f, winSize.height - 12.f});
        menu->addChild(btn);

        this->addChild(menu, 100);
    }

    void onFMOpen(CCObject*) {
        if (g_popup) return;
        auto popup = FMPopup::create();
        if (!popup) return;
        this->addChild(popup, 2000);
    }
};
