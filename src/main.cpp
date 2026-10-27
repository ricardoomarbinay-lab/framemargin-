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

// BEGIN METER
// Medidor de ventanas POR ARO (sin tipos de GD, se puede testear aparte)
struct RingWin { bool in = false; int entry = 0; int click = -1; };
struct MarginEvent { int ring; int w; int react; };
struct Meter {
    std::vector<RingWin> rings;
    bool pendingDown = false;
    int unclicked = 0;   // ventanas que se cerraron sin ningun click

    void reset(size_t n) { rings.assign(n, RingWin{}); pendingDown = false; }
    void clear() { for (auto& r : rings) r = RingWin{}; pendingDown = false; }
    void onDown() { pendingDown = true; }

    // Llamar UNA vez por step, DESPUES de la fisica.
    // ov[i] = la hitbox del jugador toca el aro i en este step.
    void step(int frame, std::vector<char> const& ov, std::vector<MarginEvent>& out) {
        size_t n = rings.size();
        if (ov.size() != n) return;
        if (pendingDown) {
            // el click activa el aro mas antiguo que ya estaba tocando...
            int best = -1;
            for (size_t i = 0; i < n; ++i) {
                if (rings[i].in && rings[i].click < 0 &&
                    (best < 0 || rings[i].entry < rings[best].entry)) best = int(i);
            }
            // ...o, si no habia ninguno, el que se empieza a tocar en este mismo step
            if (best < 0) {
                for (size_t i = 0; i < n; ++i) {
                    if (!rings[i].in && ov[i]) { best = int(i); break; }
                }
                if (best >= 0) { rings[best].in = true; rings[best].entry = frame; }
            }
            if (best >= 0) rings[best].click = frame;
            pendingDown = false;
        }
        for (size_t i = 0; i < n; ++i) {
            if (ov[i] && !rings[i].in) {
                rings[i].in = true; rings[i].entry = frame; rings[i].click = -1;
            } else if (!ov[i] && rings[i].in) {
                if (rings[i].click >= 0) {
                    out.push_back({int(i), frame - rings[i].entry, rings[i].click - rings[i].entry});
                } else {
                    unclicked++;
                }
                rings[i] = RingWin{};
            }
        }
    }
    bool anyIn() const { for (auto const& r : rings) if (r.in) return true; return false; }
    int lastClick() const { int c = -1; for (auto const& r : rings) if (r.in && r.click > c) c = r.click; return c; }
};
// END METER

struct State {
    // contadores 1..20
    std::array<int, MAX_W + 1> counts{};
    std::array<CCLabelBMFont*, MAX_W + 1> labels{};
    CCLabelBMFont* reactionLabel = nullptr;
    std::string lastReact = "React: -- ms";

    // aros del nivel (cache)
    std::vector<RingObject*> rings;

    // ventana del aro
    bool inWindow = false;
    int entryFrame = 0;
    int clickFrame = -1;
    int lastFrame = -1;
    Meter meter;                 // ventanas por aro
    std::vector<char> ov;        // solapamiento por aro (reutilizado cada step)
    int stepFrame = 0;           // frame de referencia del step actual (grabar/reproducir/medir)
    int lastDeathFrame = -1;     // diagnostico: frame de la ultima muerte en replay
    int touchNow = 0;            // aros que GD dice que toca el jugador ahora
    int lastTouch = 0;
    int winClicked = 0;          // ventanas de aro con click
    int winOut = 0;              // ventanas con click fuera del rango de margen
    int lastRawW = 0;
    int logBudget = 300;         // limite de lineas de log por reproduccion

    // macro (modo practica)
    std::vector<Input> macro;
    bool recording = false;
    bool playing = false;
    size_t playIdx = 0;
    bool injecting = false;
    bool syncPending = false;
    int injected = 0;
    int measured = 0;

    // ---- opciones numericas (se guardan) ----
    int minW = 1;          // margen minimo a contar
    int maxW = 20;         // margen maximo a contar
    int tps = 240;
    int volume = 100;      // %
    int hudScale = 100;    // %
    int hudOpacity = 100;  // %
    int circleSize = 6;
    int ringBox = 100;     // % de la hitbox del aro (calibracion)
    int hudX = 8;          // posicion X del HUD
    int hudY = 60;         // distancia desde arriba (evita pisar los textos de GD)

    // ---- opciones on/off (se guardan) ----
    bool soundOn = true;
    bool circlesOn = true;
    bool autoRestart = true;
    bool showReact = true;
    bool useGeometry = false;  // deteccion EXTRA por hitbox propia (los aros de GD siempre se usan)

    bool hudRestore = false;
    bool resetting = false;    // true mientras GD hace resetLevel (no grabar inputs)
} S;

// ---- definicion de opciones editables desde el menu ----
struct StepDef {
    char const* name;
    int* val;
    int mn, mx, step;
    char const* suffix;
};
static StepDef g_steps[] = {
    {"Margen minimo",   &S.minW,       1,  20,   1,  "f"},
    {"Margen maximo",   &S.maxW,       1,  20,   1,  "f"},
    {"TPS del juego",   &S.tps,        60, 1000, 20, ""},
    {"Volumen",         &S.volume,     0,  100,  5,  "%"},
    {"Escala HUD",      &S.hudScale,   50, 200,  10, "%"},
    {"Opacidad HUD",    &S.hudOpacity, 10, 100,  10, "%"},
    {"Tam. circulo",    &S.circleSize, 2,  20,   1,  ""},
    {"Hitbox del aro",  &S.ringBox,    50, 150,  5,  "%"},
    {"HUD posicion X",  &S.hudX,       0,  400,  5,  ""},
    {"HUD posicion Y",  &S.hudY,       0,  200,  5,  ""},
};
constexpr int N_STEPS = sizeof(g_steps) / sizeof(g_steps[0]);

struct ToggleDef {
    char const* name;
    bool* val;
};
static ToggleDef g_toggles[] = {
    {"Sonido",                    &S.soundOn},
    {"Circulos",                  &S.circlesOn},
    {"Auto-reiniciar al reproducir", &S.autoRestart},
    {"Mostrar reaccion (ms)",     &S.showReact},
    {"Hitbox propia (extra)",     &S.useGeometry},
};

// ============================================================
//  Utilidades
// ============================================================
template <class T>
static bool hasItems(T const& r) {
    if constexpr (requires { r.size(); }) return r.size() > 0;
    else return r && r->count() > 0;
}

template <class T>
static int itemCount(T const& r) {
    if constexpr (requires { r.size(); }) return int(r.size());
    else return r ? int(r->count()) : 0;
}

template <class L, class F>
static void forEachTouching(L const& l, F&& fn) {
    if constexpr (requires { l->count(); }) {
        if (l) for (auto o : CCArrayExt<RingObject*>(l)) fn(o);
    } else {
        for (auto o : l) fn(static_cast<RingObject*>(o));
    }
}

template <class T>
static int asInt(T& v) {
    if constexpr (requires { v.value(); }) return static_cast<int>(v.value());
    else return static_cast<int>(v);
}

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
    return 1000.f / float(std::max(1, S.tps));
}

static void clampCfg() {
    for (auto& d : g_steps) *d.val = std::clamp(*d.val, d.mn, d.mx);
    if (S.minW > S.maxW) S.maxW = S.minW;
}

static void loadCfg() {
    auto m = Mod::get();
    for (int i = 0; i < N_STEPS; ++i) {
        *g_steps[i].val = m->getSavedValue<int>(fmt::format("step_{}", i), *g_steps[i].val);
    }
    for (int i = 0; i < int(std::size(g_toggles)); ++i) {
        *g_toggles[i].val = m->getSavedValue<bool>(fmt::format("t3_{}", i), *g_toggles[i].val);
    }
    clampCfg();
}

static void saveCfg() {
    auto m = Mod::get();
    for (int i = 0; i < N_STEPS; ++i) m->setSavedValue<int>(fmt::format("step_{}", i), *g_steps[i].val);
    for (int i = 0; i < int(std::size(g_toggles)); ++i) m->setSavedValue<bool>(fmt::format("t3_{}", i), *g_toggles[i].val);
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

// Guardado / carga silenciosa de la replay del nivel actual
static bool writeMacro() {
    if (S.macro.empty()) return false;
    std::ofstream f(macroPath());
    if (!f) return false;
    for (auto const& in : S.macro) {
        f << in.frame << ' ' << (in.down ? 1 : 0) << ' ' << in.button << ' ' << (in.p1 ? 1 : 0) << '\n';
    }
    return true;
}

static bool readMacro() {
    std::ifstream f(macroPath());
    if (!f) return false;
    std::vector<Input> loaded;
    int a, b, c, d;
    while (f >> a >> b >> c >> d) loaded.push_back({a, b != 0, c, d != 0});
    if (loaded.empty()) return false;
    S.macro = std::move(loaded);
    return true;
}

$execute {
    loadCfg();
    log::info("[FrameMargin] v1.5.3 cargado");
}

static std::string statusText() {
    if (S.recording) return fmt::format("Grabando ({} inputs)", S.macro.size());
    if (S.playing) return "Reproduciendo";
    return "Inactivo";
}

// ============================================================
//  HUD
// ============================================================
static void applyHud() {
    bool vis = S.playing;   // el HUD SOLO se ve al reproducir la replay
    float k = S.hudScale / 100.f;
    float y = CCDirector::get()->getWinSize().height - float(S.hudY);
    GLubyte op = (GLubyte)(255 * S.hudOpacity / 100);
    for (int w = 1; w <= MAX_W; ++w) {
        auto l = S.labels[w];
        if (!l) continue;
        bool in = (w >= S.minW && w <= S.maxW);
        l->setVisible(vis && in);
        if (in) {
            l->setScale(0.32f * k);
            l->setPosition({float(S.hudX), y});
            l->setOpacity(op);
            y -= 11.5f * k;
        }
    }
    if (S.reactionLabel) {
        S.reactionLabel->setVisible(vis && S.showReact);
        S.reactionLabel->setScale(0.36f * k);
        S.reactionLabel->setPosition({float(S.hudX), y - 6.f});
        S.reactionLabel->setOpacity(op);
    }
}

static void hideHud() {
    for (int w = 1; w <= MAX_W; ++w) if (S.labels[w]) S.labels[w]->setVisible(false);
    if (S.reactionLabel) S.reactionLabel->setVisible(false);
}

static void refreshPopup();

static void refreshCounts() {
    S.lastReact = "React: -- ms";
    for (int w = 1; w <= MAX_W; ++w) {
        if (S.labels[w]) S.labels[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
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
    applyHud();
    refreshPopup();
    S.inWindow = false;
    S.lastFrame = -1;
    Notification::create("Grabando desde el inicio del nivel", NotificationIcon::Success)->show();
}

static void fmStop() {
    bool wasRec = S.recording;
    S.recording = false;
    S.playing = false;
    applyHud();
    refreshPopup();
    if (wasRec && !S.macro.empty()) {
        bool ok = writeMacro();   // se guarda sola en disco
        Notification::create(fmt::format("{} inputs {}", S.macro.size(), ok ? "grabados y guardados" : "grabados"),
            NotificationIcon::Success)->show();
    } else {
        Notification::create("Detenido", NotificationIcon::Info)->show();
    }
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
    S.lastFrame = -1;
    S.meter.clear();
    S.meter.unclicked = 0;
    S.lastDeathFrame = -1;
    S.winClicked = 0; S.winOut = 0; S.lastRawW = 0; S.lastTouch = 0; S.touchNow = 0; S.logBudget = 300;
    log::info("[FrameMargin] reproduciendo: {} inputs, primer frame={}, ultimo frame={}",
        S.macro.size(), S.macro.front().frame, S.macro.back().frame);
    refreshCounts();
    applyHud();
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
    applyHud();
    refreshPopup();
    Notification::create(fmt::format("Cargada ({} inputs)", S.macro.size()), NotificationIcon::Success)->show();
}

// ============================================================
//  Medicion
// ============================================================
// Solapamiento de la hitbox del jugador con UN aro (con calibracion %)
static bool overlapsOne(CCRect const& pr, RingObject* ring) {
    if (!ring) return false;
    if (std::fabs(ring->getPositionX() - pr.getMidX()) > 70.f) return false;
    float k = S.ringBox / 100.f;
    auto rr = ring->getObjectRect();
    float nw = rr.size.width * k, nh = rr.size.height * k;
    rr.origin.x += (rr.size.width - nw) / 2.f;
    rr.origin.y += (rr.size.height - nh) / 2.f;
    rr.size.width = nw;
    rr.size.height = nh;
    return pr.intersectsRect(rr);
}

static void onMarginFound(GJBaseGameLayer* gl, int w, float reactionMs) {
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
        vol *= S.volume / 100.f;
        float speed = 1.5f - (w - 1) * 0.04f;
        auto custom = soundFor(w);
        if (!custom.empty()) {
            FMODAudioEngine::sharedEngine()->playEffect(custom.string(), 1.f, 0.f, vol);
        } else {
            FMODAudioEngine::sharedEngine()->playEffect("playSound_01.ogg", speed, 0.f, vol);
        }
    }

    // Circulito de color
    if (S.circlesOn && gl->m_objectLayer && gl->m_player1) {
        auto dot = CCDrawNode::create();
        auto c = colorFor(w);
        dot->drawDot({0, 0}, float(S.circleSize), {c.r / 255.f, c.g / 255.f, c.b / 255.f, 0.9f});
        dot->setPosition(gl->m_player1->getPosition());
        dot->setZOrder(100);
        gl->m_objectLayer->addChild(dot);
    }
}

// ============================================================
//  PlayLayer
// ============================================================
class $modify(FMPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        log::info("[FrameMargin] PlayLayer::setupHasCompleted -> creando HUD");

        S.counts.fill(0);
        S.inWindow = false;
        S.clickFrame = -1;
        S.lastFrame = -1;
        S.measured = 0;
        S.recording = false;
        S.playing = false;
        S.resetting = false;
        if (!readMacro()) S.macro.clear();   // replay guardada de ESTE nivel

        // cachear los aros del nivel una sola vez
        S.rings.clear();
        if (m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (auto r = typeinfo_cast<RingObject*>(obj)) S.rings.push_back(r);
            }
        }
        log::info("[FrameMargin] aros detectados: {}", S.rings.size());
        S.meter.reset(S.rings.size());
        S.ov.assign(S.rings.size(), 0);
        S.lastDeathFrame = -1;

        for (int w = 1; w <= MAX_W; ++w) {
            auto lbl = CCLabelBMFont::create(fmt::format("{}: 0", w).c_str(), "bigFont.fnt");
            lbl->setAnchorPoint({0, 0.5f});
            lbl->setColor(colorFor(w));
            lbl->setZOrder(1000);
            this->addChild(lbl);
            S.labels[w] = lbl;
        }
        S.reactionLabel = CCLabelBMFont::create("React: -- ms", "bigFont.fnt");
        S.reactionLabel->setAnchorPoint({0, 0.5f});
        S.reactionLabel->setZOrder(1000);
        this->addChild(S.reactionLabel);

        applyHud();
        Notification::create("Frame Margin v1.5.3 activo", NotificationIcon::Success)->show();
    }

    void resetLevel() {
        S.resetting = true;
        PlayLayer::resetLevel();
        S.resetting = false;
        S.inWindow = false;
        S.clickFrame = -1;
        S.lastFrame = -1;
        S.playIdx = 0;
        S.syncPending = true;
        S.meter.clear();
        int f = m_gameState.m_currentProgress;
        S.stepFrame = f;
        int cps = itemCount(m_checkpointArray);
        // Grabando en practica: al reaparecer se descartan los inputs desde ese
        // frame (o todo, si no queda ningun checkpoint) -> queda solo el camino bueno.
        if (S.recording) {
            if (cps == 0) S.macro.clear();
            else std::erase_if(S.macro, [f](Input const& in) { return in.frame >= f; });
        }
        log::info("[FrameMargin] resetLevel: frame={} checkpoints={} macro={} grabando={} reproduciendo={}",
            f, cps, S.macro.size(), S.recording, S.playing);
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool alive = m_player1 && !m_player1->m_isDead;
        PlayLayer::destroyPlayer(player, obj);
        if (!S.playing || !alive || player != m_player1 || !m_player1->m_isDead) return;

        S.lastDeathFrame = m_gameState.m_currentProgress;
        bool finished = S.playIdx >= S.macro.size();
        log::info("[FrameMargin] muerte en replay: frame={} input={}/{} fin={}",
            S.lastDeathFrame, S.playIdx, S.macro.size(), finished);

        // en vez de morir en bucle, se corta la replay y se avisa
        S.playing = false;
        S.meter.clear();
        S.inWindow = false;
        applyHud();
        refreshPopup();
        std::string msg = finished
            ? std::string("Replay terminada (fin de lo grabado)")
            : fmt::format("Replay desincronizada: murio en frame {} (input {}/{})",
                S.lastDeathFrame, S.playIdx, S.macro.size());
        Notification::create(msg, finished ? NotificationIcon::Success : NotificationIcon::Error)->show();
    }

    void levelComplete() {
        if (S.recording) {
            S.recording = false;
            writeMacro();
            Notification::create(fmt::format("Replay guardada ({} inputs)", S.macro.size()), NotificationIcon::Success)->show();
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (S.recording) {
            S.recording = false;
            writeMacro();
        }
        S.labels.fill(nullptr);
        S.reactionLabel = nullptr;
        S.rings.clear();
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
        bool active = PlayLayer::get() && m_player1;

        if (active) {
            // UN solo frame de referencia por step: grabacion, reproduccion y
            // medicion lo comparten, asi nunca quedan desfasados entre si.
            S.stepFrame = m_gameState.m_currentProgress;

            // reproducir macro ANTES del paso de fisica (mismo punto donde GD
            // procesa los clicks reales)
            if (S.playing) {
                int pf = S.stepFrame;
                if (S.syncPending) {
                    while (S.playIdx < S.macro.size() && S.macro[S.playIdx].frame < pf) S.playIdx++;
                    S.syncPending = false;
                }
                while (S.playIdx < S.macro.size() && S.macro[S.playIdx].frame <= pf) {
                    auto const& in = S.macro[S.playIdx];
                    S.injecting = true;
                    this->handleButton(in.down, in.button, in.p1);
                    S.injecting = false;
                    S.playIdx++;
                    S.injected++;
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!active) return;

        if (S.hudRestore) {
            S.hudRestore = false;
            applyHud();
        }

        // el margen SOLO se mide durante la reproduccion
        if (!S.playing) {
            S.meter.clear();
            S.inWindow = false;
            return;
        }

        // una sola medicion por frame
        if (S.stepFrame == S.lastFrame) return;
        S.lastFrame = S.stepFrame;

        if (m_player1->m_isDead) {
            S.meter.clear();
            S.inWindow = false;
            return;
        }

        // aros que GD dice que toca el jugador AHORA (colision exacta de GD)
        std::vector<RingObject*> touching;
        forEachTouching(m_player1->m_touchingRings, [&](RingObject* r) { if (r) touching.push_back(r); });
        S.touchNow = int(touching.size());

        // registrar aros nuevos al vuelo (no depende de escanear el nivel)
        for (auto r : touching) {
            if (std::find(S.rings.begin(), S.rings.end(), r) == S.rings.end()) S.rings.push_back(r);
        }
        size_t n = S.rings.size();
        if (S.meter.rings.size() < n) S.meter.rings.resize(n);
        S.ov.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            if (std::find(touching.begin(), touching.end(), S.rings[i]) != touching.end()) S.ov[i] = 1;
        }
        // deteccion extra opcional por hitbox propia
        if (S.useGeometry) {
            auto pr = m_player1->getObjectRect();
            for (size_t i = 0; i < n; ++i) if (overlapsOne(pr, S.rings[i])) S.ov[i] = 1;
        }

        if (S.touchNow != S.lastTouch) {
            if (S.logBudget > 0) {
                S.logBudget--;
                log::info("[FrameMargin] frame {}: GD toca {} aro(s)", S.stepFrame, S.touchNow);
            }
            S.lastTouch = S.touchNow;
        }

        std::vector<MarginEvent> evs;
        S.meter.step(S.stepFrame, S.ov, evs);
        S.inWindow = S.meter.anyIn();
        S.clickFrame = S.meter.lastClick();

        for (auto const& e : evs) {
            S.winClicked++;
            S.lastRawW = e.w;
            bool ok = e.w >= S.minW && e.w <= S.maxW && e.w <= MAX_W;
            if (S.logBudget > 0) {
                S.logBudget--;
                log::info("[FrameMargin] ventana aro#{}: w={} react={} frame={} -> {}",
                    e.ring, e.w, e.react, S.stepFrame, ok ? "CONTADA" : "fuera de rango");
            }
            if (ok) onMarginFound(this, e.w, e.react * msPerFrame());
            else S.winOut++;
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (!PlayLayer::get()) return;

        if (S.recording && !S.injecting && !S.resetting) {
            S.macro.push_back({S.stepFrame, down, button, isPlayer1});
        }
        if (down && isPlayer1 && S.playing) {
            S.meter.onDown();
        }
    }
};

// ============================================================
//  Menu con categorias
// ============================================================
class FMPopup;
static FMPopup* g_popup = nullptr;

class FMPopup : public CCLayer {
public:
    static constexpr int NTABS = 6;
    static constexpr float W = 440.f;
    static constexpr float H = 290.f;

    std::array<CCNode*, NTABS> m_pages{};
    std::array<ButtonSprite*, NTABS> m_tabSprs{};
    std::array<CCLabelBMFont*, 16> m_stepVal{};
    std::array<CCLabelBMFont*, MAX_W + 1> m_counts{};
    std::array<CCLabelBMFont*, 9> m_dbg{};
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_react = nullptr;
    CCNode* m_panel = nullptr;
    int m_tab = 0;

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

    CCLabelBMFont* addLabel(CCNode* parent, char const* txt, char const* font, float scale,
                            float x, float y, bool center) {
        auto l = CCLabelBMFont::create(txt, font);
        l->setScale(scale);
        if (!center) l->setAnchorPoint({0, 0.5f});
        l->setPosition({x, y});
        parent->addChild(l);
        return l;
    }

    CCMenu* newPage(int idx) {
        auto root = CCNode::create();
        root->setContentSize({W, H});
        root->setPosition({0, 0});
        m_panel->addChild(root, 3);
        m_pages[idx] = root;
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        menu->setTouchPriority(-710);
        root->addChild(menu, 5);
        return menu;
    }

    void addButton(CCMenu* menu, char const* txt, char const* tex, float x, float y, float w, SEL_MenuHandler cb) {
        auto spr = ButtonSprite::create(txt, int(w), true, "bigFont.fnt", tex, 26.f, 0.5f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        btn->setPosition({x, y});
        menu->addChild(btn);
    }

    void addToggleRow(CCNode* root, CCMenu* menu, int id, float y) {
        auto t = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(FMPopup::onToggle), 0.6f);
        t->setTag(id);
        t->toggle(*g_toggles[id].val);
        t->setPosition({130.f, y});
        menu->addChild(t);
        addLabel(root, g_toggles[id].name, "bigFont.fnt", 0.38f, 150.f, y, false);
    }

    void addStepRow(CCNode* root, CCMenu* menu, int id, float y) {
        addLabel(root, g_steps[id].name, "bigFont.fnt", 0.38f, 122.f, y, false);
        auto mk = [&](char const* t, int tag, float x) {
            auto spr = ButtonSprite::create(t, 26, true, "bigFont.fnt", "GJ_button_01.png", 24.f, 0.7f);
            auto b = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FMPopup::onStep));
            b->setTag(tag);
            b->setPosition({x, y});
            menu->addChild(b);
        };
        mk("-", id * 2, 296.f);
        mk("+", id * 2 + 1, 384.f);
        m_stepVal[id] = addLabel(root, "", "bigFont.fnt", 0.42f, 340.f, y, true);
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        g_popup = this;

        auto winSize = CCDirector::get()->getWinSize();
        float cx = winSize.width / 2;
        float cy = winSize.height / 2;

        auto dim = CCLayerColor::create({0, 0, 0, 160});
        this->addChild(dim, 0);

        // bloquea toques fuera del panel
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
        m_panel = CCNode::create();
        m_panel->setContentSize({W, H});
        m_panel->setAnchorPoint({0.5f, 0.5f});
        m_panel->ignoreAnchorPointForPosition(false);
        m_panel->setPosition({cx, cy});
        this->addChild(m_panel, 2);

        if (auto bg = CCScale9Sprite::create("GJ_square01.png")) {
            bg->setContentSize({W, H});
            bg->setAnchorPoint({0, 0});
            bg->setPosition({0, 0});
            m_panel->addChild(bg, -1);
        }
        // separador de la barra lateral
        if (auto sep = CCScale9Sprite::create("square02_001.png")) {
            sep->setContentSize({2.f, H - 60.f});
            sep->setOpacity(80);
            sep->setPosition({108.f, (H - 30.f) / 2});
            m_panel->addChild(sep, 0);
        }

        addLabel(m_panel, "Frame Margin", "goldFont.fnt", 0.8f, W / 2, H - 16.f, true);

        // menu principal: cerrar + pestanas
        auto mainMenu = CCMenu::create();
        mainMenu->setPosition({0, 0});
        mainMenu->setTouchPriority(-710);
        m_panel->addChild(mainMenu, 6);

        auto closeSpr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        auto closeBtn = CCMenuItemSpriteExtra::create(closeSpr, this, menu_selector(FMPopup::onClose));
        closeBtn->setPosition({10.f, H - 10.f});
        mainMenu->addChild(closeBtn);

        char const* tabNames[NTABS] = {"Replay", "Margenes", "Sonido", "HUD", "Medicion", "Debug"};
        for (int i = 0; i < NTABS; ++i) {
            auto spr = ButtonSprite::create(tabNames[i], 90, true, "bigFont.fnt",
                i == 0 ? "GJ_button_02.png" : "GJ_button_01.png", 26.f, 0.5f);
            m_tabSprs[i] = spr;
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FMPopup::onTab));
            btn->setTag(i);
            btn->setPosition({56.f, H - 56.f - i * 34.f});
            mainMenu->addChild(btn);
        }

        // ---------------- Pagina 0: REPLAY ----------------
        {
            auto menu = newPage(0);
            auto root = m_pages[0];
            addLabel(root, "REPLAY", "goldFont.fnt", 0.6f, 271.f, H - 42.f, true);
            m_status = addLabel(root, "", "bigFont.fnt", 0.4f, 271.f, H - 62.f, true);
            m_info = addLabel(root, "", "bigFont.fnt", 0.3f, 271.f, H - 78.f, true);
            addButton(menu, "Grabar",     "GJ_button_01.png", 196.f, H - 108.f, 120.f, menu_selector(FMPopup::onRecord));
            addButton(menu, "Parar",      "GJ_button_04.png", 346.f, H - 108.f, 120.f, menu_selector(FMPopup::onStop));
            addButton(menu, "Reproducir", "GJ_button_01.png", 196.f, H - 144.f, 120.f, menu_selector(FMPopup::onPlay));
            addButton(menu, "Reset",      "GJ_button_04.png", 346.f, H - 144.f, 120.f, menu_selector(FMPopup::onReset));
            addButton(menu, "Guardar",    "GJ_button_01.png", 196.f, H - 180.f, 120.f, menu_selector(FMPopup::onSave));
            addButton(menu, "Cargar",     "GJ_button_01.png", 346.f, H - 180.f, 120.f, menu_selector(FMPopup::onLoad));
            addToggleRow(root, menu, 2, H - 222.f);
            addLabel(root, "Grabar (solo en practica) y Reproducir reinician", "bigFont.fnt", 0.26f, 122.f, H - 248.f, false);
            addLabel(root, "el nivel desde el inicio. Parar guarda la replay.", "bigFont.fnt", 0.26f, 122.f, H - 261.f, false);
        }

        // ---------------- Pagina 1: MARGENES ----------------
        {
            auto menu = newPage(1);
            auto root = m_pages[1];
            addLabel(root, "MARGENES", "goldFont.fnt", 0.6f, 271.f, H - 42.f, true);
            addStepRow(root, menu, 0, H - 72.f);
            addStepRow(root, menu, 1, H - 100.f);
            addLabel(root, "Solo se cuentan margenes dentro del rango.", "bigFont.fnt", 0.26f, 122.f, H - 122.f, false);
            for (int w = 1; w <= MAX_W; ++w) {
                int col = (w - 1) % 4;
                int row = (w - 1) / 4;
                float x = 124.f + col * 76.f;
                float y = H - 146.f - row * 16.f;
                auto l = addLabel(root, "", "bigFont.fnt", 0.36f, x, y, false);
                l->setColor(colorFor(w));
                m_counts[w] = l;
            }
            m_react = addLabel(root, "", "bigFont.fnt", 0.34f, 122.f, H - 258.f, false);
            addButton(menu, "Reset contadores", "GJ_button_04.png", 350.f, H - 256.f, 130.f, menu_selector(FMPopup::onReset));
        }

        // ---------------- Pagina 2: SONIDO ----------------
        {
            auto menu = newPage(2);
            auto root = m_pages[2];
            addLabel(root, "SONIDO", "goldFont.fnt", 0.6f, 271.f, H - 42.f, true);
            addToggleRow(root, menu, 0, H - 80.f);
            addStepRow(root, menu, 3, H - 114.f);
            addLabel(root, "Menos frames = mas fuerte y mas agudo.", "bigFont.fnt", 0.28f, 122.f, H - 156.f, false);
            addLabel(root, "Sonidos propios (opcional), carpeta:", "bigFont.fnt", 0.28f, 122.f, H - 182.f, false);
            addLabel(root, "geode/config/tuusuario.framemargin/sounds", "bigFont.fnt", 0.26f, 122.f, H - 197.f, false);
            addLabel(root, "Nombres: frame_1.ogg ... frame_20.ogg", "bigFont.fnt", 0.28f, 122.f, H - 212.f, false);
            addLabel(root, "(tambien .mp3 o .wav)", "bigFont.fnt", 0.28f, 122.f, H - 227.f, false);
        }

        // ---------------- Pagina 3: HUD ----------------
        {
            auto menu = newPage(3);
            auto root = m_pages[3];
            addLabel(root, "HUD", "goldFont.fnt", 0.6f, 271.f, H - 42.f, true);
            addToggleRow(root, menu, 3, H - 70.f);
            addToggleRow(root, menu, 1, H - 94.f);
            addStepRow(root, menu, 4, H - 126.f);
            addStepRow(root, menu, 5, H - 152.f);
            addStepRow(root, menu, 6, H - 178.f);
            addStepRow(root, menu, 8, H - 204.f);
            addStepRow(root, menu, 9, H - 230.f);
            addLabel(root, "El HUD solo aparece al reproducir la replay.", "bigFont.fnt", 0.26f, 122.f, H - 256.f, false);
        }

        // ---------------- Pagina 4: MEDICION ----------------
        {
            auto menu = newPage(4);
            auto root = m_pages[4];
            addLabel(root, "MEDICION", "goldFont.fnt", 0.6f, 271.f, H - 42.f, true);
            addToggleRow(root, menu, 4, H - 80.f);
            addStepRow(root, menu, 2, H - 118.f);
            addStepRow(root, menu, 7, H - 148.f);
            addLabel(root, "Los aros se detectan con la colision de GD.", "bigFont.fnt", 0.26f, 122.f, H - 198.f, false);
            addLabel(root, "Hitbox propia = deteccion extra (usa el % de abajo).", "bigFont.fnt", 0.26f, 122.f, H - 211.f, false);
            addLabel(root, "Ventanas mayores al margen maximo se descartan", "bigFont.fnt", 0.26f, 122.f, H - 230.f, false);
            addLabel(root, "(ver pestana Debug: Fuera de rango).", "bigFont.fnt", 0.26f, 122.f, H - 243.f, false);
        }

        // ---------------- Pagina 5: DEBUG ----------------
        {
            newPage(5);
            auto root = m_pages[5];
            addLabel(root, "DEBUG", "goldFont.fnt", 0.6f, 271.f, H - 42.f, true);
            for (int i = 0; i < 9; ++i) {
                m_dbg[i] = addLabel(root, "", "bigFont.fnt", 0.34f, 124.f, H - 72.f - i * 20.f, false);
            }
        }

        selectTab(0);
        refresh();
        this->schedule(schedule_selector(FMPopup::tick), 0.2f);
        return true;
    }

    void tick(float) { refresh(); }

    void selectTab(int i) {
        m_tab = i;
        for (int k = 0; k < NTABS; ++k) {
            if (m_pages[k]) m_pages[k]->setVisible(k == i);
            if (m_tabSprs[k]) m_tabSprs[k]->updateBGImage(k == i ? "GJ_button_02.png" : "GJ_button_01.png");
        }
    }

    void refresh() {
        if (m_status) m_status->setString(fmt::format("Estado: {}", statusText()).c_str());
        if (m_info) {
            m_info->setString(fmt::format("Replay: {} inputs | Inyectados: {} | Medidos: {}",
                S.macro.size(), S.injected, S.measured).c_str());
        }
        for (int w = 1; w <= MAX_W; ++w) {
            if (!m_counts[w]) continue;
            m_counts[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
            m_counts[w]->setOpacity((w >= S.minW && w <= S.maxW) ? 255 : 70);
        }
        if (m_react) m_react->setString(S.lastReact.c_str());
        for (int i = 0; i < N_STEPS && i < 16; ++i) {
            if (m_stepVal[i]) {
                m_stepVal[i]->setString(fmt::format("{}{}", *g_steps[i].val, g_steps[i].suffix).c_str());
            }
        }
        if (m_dbg[0]) {
            m_dbg[0]->setString(fmt::format("Aros registrados: {} | GD toca ahora: {}", S.rings.size(), S.touchNow).c_str());
            m_dbg[1]->setString(fmt::format("En ventana: {} | click: {}", S.inWindow ? "SI" : "no", S.clickFrame >= 0 ? "SI" : "no").c_str());
            m_dbg[2]->setString(fmt::format("Frame: {} | muerte replay: {}", S.lastFrame, S.lastDeathFrame).c_str());
            m_dbg[3]->setString(fmt::format("Ventanas con click: {} | contadas: {}", S.winClicked, S.measured).c_str());
            m_dbg[4]->setString(fmt::format("Fuera de rango: {} (ultimo w={})", S.winOut, S.lastRawW).c_str());
            m_dbg[5]->setString(fmt::format("Ventanas sin click: {}", S.meter.unclicked).c_str());
            m_dbg[6]->setString(fmt::format("Inyectados: {} de {}", S.injected, S.macro.size()).c_str());
            m_dbg[7]->setString(fmt::format("Rango: {}-{}f | TPS: {}", S.minW, S.maxW, S.tps).c_str());
            m_dbg[8]->setString(fmt::format("Hitbox propia extra: {}", S.useGeometry ? "SI" : "no").c_str());
        }
    }

    void onNoop(CCObject*) {}
    void onClose(CCObject*) { this->removeFromParentAndCleanup(true); }
    void onTab(CCObject* s) { selectTab(static_cast<CCNode*>(s)->getTag()); }

    void onStep(CCObject* s) {
        int tag = static_cast<CCNode*>(s)->getTag();
        int id = tag / 2;
        int dir = (tag % 2) ? 1 : -1;
        if (id < 0 || id >= N_STEPS) return;
        auto& d = g_steps[id];
        *d.val = std::clamp(*d.val + dir * d.step, d.mn, d.mx);
        if (id == 0 && S.minW > S.maxW) S.maxW = S.minW;
        if (id == 1 && S.maxW < S.minW) S.minW = S.maxW;
        saveCfg();
        applyHud();
        refresh();
    }

    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        int id = t->getTag();
        if (id < 0 || id >= int(std::size(g_toggles))) return;
        *g_toggles[id].val = !t->isToggled();
        saveCfg();
        applyHud();
        refresh();
    }

    void onRecord(CCObject*) {
        fmRecord();
        refresh();
        // empezar a grabar desde el frame 0 para capturar todo el camino
        if (S.recording) {
            if (auto pause = typeinfo_cast<PauseLayer*>(this->getParent())) {
                pause->onRestartFull(nullptr);
            }
        }
    }
    void onStop(CCObject*) { fmStop(); refresh(); }
    void onReset(CCObject*) { fmReset(); refresh(); }
    void onSave(CCObject*) { fmSave(); refresh(); }
    void onLoad(CCObject*) { fmLoad(); refresh(); }

    void onPlay(CCObject*) {
        fmPlay();
        refresh();
        if (S.playing && S.autoRestart) {
            if (auto pause = typeinfo_cast<PauseLayer*>(this->getParent())) {
                pause->onRestartFull(nullptr);   // reproducir desde el inicio
            }
        }
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

        hideHud();
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
