#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <array>
#include <vector>

using namespace geode::prelude;

constexpr int MAX_W = 20;

// ---------- Estado global ----------
struct State {
    std::array<int, MAX_W + 1> counts{};      // counts[1..20]
    std::array<CCLabelBMFont*, MAX_W + 1> labels{};
    CCLabelBMFont* reactionLabel = nullptr;

    // ventana del aro
    bool inWindow = false;
    int entryFrame = 0;
    int clickFrame = -1;

    // macro (practica)
    struct Input { int frame; bool down; };
    std::vector<Input> macro;
    bool recording = false;
    bool playing = false;
    size_t playIdx = 0;
    bool injecting = false; // evita regrabar los clicks que inyectamos
} S;

// Color por margen: 1 = rojo ... 20 = violeta
static ccColor3B colorFor(int w) {
    float h = (w - 1) / float(MAX_W) * 270.f; // 0..270 grados
    float c = 1.f, x = c * (1 - fabsf(fmodf(h / 60.f, 2) - 1));
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

// ---------- PlayLayer: HUD ----------
class $modify(FMPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        S.counts.fill(0);
        S.inWindow = false;

        auto winSize = CCDirector::get()->getWinSize();
        float y = winSize.height - 20.f;
        for (int w = 1; w <= MAX_W; ++w) {
            auto lbl = CCLabelBMFont::create(fmt::format("{}: 0", w).c_str(), "bigFont.fnt");
            lbl->setScale(0.35f);
            lbl->setAnchorPoint({0, 0.5f});
            lbl->setPosition({8.f, y});
            lbl->setColor(colorFor(w));
            lbl->setZOrder(1000);
            this->addChild(lbl);
            S.labels[w] = lbl;
            y -= 13.f;
        }
        S.reactionLabel = CCLabelBMFont::create("React: -- ms", "bigFont.fnt");
        S.reactionLabel->setScale(0.4f);
        S.reactionLabel->setAnchorPoint({0, 0.5f});
        S.reactionLabel->setPosition({8.f, y - 8.f});
        S.reactionLabel->setZOrder(1000);
        this->addChild(S.reactionLabel);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        S.inWindow = false;
        S.clickFrame = -1;
        S.playIdx = 0;
        if (S.recording) S.macro.clear(); // regrabar desde el respawn
    }
};

// ---------- Logica por frame ----------
class $modify(FMBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
        if (!PlayLayer::get() || !m_player1) return;

        int frame = m_gameState.m_currentProgress;

        // reproducir macro
        if (S.playing) {
            while (S.playIdx < S.macro.size() && S.macro[S.playIdx].frame <= frame) {
                S.injecting = true;
                this->handleButton(S.macro[S.playIdx].down, 1, true);
                S.injecting = false;
                S.playIdx++;
            }
        }

        // ventana del aro (VERIFICAR: nombre/tipo de m_touchingRings en tu version)
        bool touching = m_player1->m_touchingRings && m_player1->m_touchingRings->count() > 0;

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
        if (!PlayLayer::get() || S.injecting) return;
        int frame = m_gameState.m_currentProgress;
        if (S.recording) S.macro.push_back({frame, down});
        if (down && S.inWindow && S.clickFrame < 0) S.clickFrame = frame;
    }

    void onMarginFound(int w, float reactionMs) {
        S.counts[w]++;
        if (S.labels[w]) S.labels[w]->setString(fmt::format("{}: {}", w, S.counts[w]).c_str());
        if (S.reactionLabel) {
            S.reactionLabel->setString(fmt::format("React: {:.1f} ms ({}f)", reactionMs, w).c_str());
            S.reactionLabel->setColor(colorFor(w));
        }

        // Sonido: menos frames = mas fuerte
        float vol = std::clamp(1.f - (w - 1) * 0.045f, 0.1f, 1.f);
        vol *= float(Mod::get()->getSettingValue<double>("volume"));
        float speed = 1.5f - (w - 1) * 0.04f; // mas agudo al ser mas dificil
        FMODAudioEngine::sharedEngine()->playEffect("playSound_01.ogg", speed, 0.f, vol);

        // Circulito donde ocurrio
        auto dot = CCDrawNode::create();
        auto c = colorFor(w);
        dot->drawDot({0, 0}, 6.f, {c.r / 255.f, c.g / 255.f, c.b / 255.f, 0.9f});
        dot->setPosition(m_player1->getPosition());
        dot->setZOrder(100);
        m_objectLayer->addChild(dot);
    }
};

// ---------- Menu en la pausa ----------
class $modify(FMPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = CCMenu::create();
        menu->setPosition({60.f, 60.f});
        menu->setLayout(ColumnLayout::create()->setAxisReverse(true));

        auto mk = [&](char const* txt, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(txt, 90, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            menu->addChild(btn);
        };
        mk("Grabar", menu_selector(FMPause::onRecord));
        mk("Parar", menu_selector(FMPause::onStop));
        mk("Reproducir", menu_selector(FMPause::onPlay));
        mk("Reset", menu_selector(FMPause::onReset));
        menu->updateLayout();
        this->addChild(menu);
    }

    void onRecord(CCObject*) {
        if (!PlayLayer::get() || !PlayLayer::get()->m_isPracticeMode)
            return FLAlertLayer::create("Frame Margin", "Activa el modo practica.", "OK")->show();
        S.macro.clear(); S.recording = true; S.playing = false;
        Notification::create("Grabando...", NotificationIcon::Success)->show();
    }
    void onStop(CCObject*) {
        S.recording = false; S.playing = false;
        Notification::create(fmt::format("{} inputs guardados", S.macro.size()), NotificationIcon::Info)->show();
    }
    void onPlay(CCObject*) {
        if (S.macro.empty()) return;
        S.recording = false; S.playing = true; S.playIdx = 0;
        Notification::create("Reproduciendo (reinicia el nivel)", NotificationIcon::Success)->show();
    }
    void onReset(CCObject*) {
        S.counts.fill(0);
        for (int w = 1; w <= MAX_W; ++w)
            if (S.labels[w]) S.labels[w]->setString(fmt::format("{}: 0", w).c_str());
    }
};
