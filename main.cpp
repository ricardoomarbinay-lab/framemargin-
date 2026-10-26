#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

constexpr int MAX_W = 20;

struct Input {
    int frame;
    bool down;
    int button;
    bool p1;
};

struct State {
    std::array<int, MAX_W + 1> counts{};
    std::array<CCLabelBMFont*, MAX_W + 1> labels{};
    CCLabelBMFont* reactionLabel = nullptr;
    bool inWindow = false;
    int entryFrame = 0;
    int clickFrame = -1;

    // macro (modo practica)
    std::vector<Input> macro;
    bool recording = false;
    bool playing = false;
    size_t playIdx = 0;
    bool injecting = false;
} S;

// Compila tanto si m_touchingRings es un vector como si es un CCArray*
template <class T>
static bool hasItems(T const& r) {
    if constexpr (requires { r.size(); }) return r.size() > 0;
    else return r && r->count() > 0;
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

        // grabar solo los clicks reales, no los que inyectamos
        if (S.recording && !S.injecting) {
            S.macro.push_back({frame, down, button, isPlayer1});
        }
        // medir reaccion tanto en clicks reales como en reproducidos
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

        // Menos frames = mas fuerte
        float vol = std::clamp(1.f - (w - 1) * 0.045f, 0.1f, 1.f);
        vol *= float(Mod::get()->getSettingValue<double>("volume"));
        float speed = 1.5f - (w - 1) * 0.04f;
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

// ---------- Menu en la pantalla de pausa ----------
class $modify(FMPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();
        menu->setContentSize({110.f, 170.f});
        menu->setPosition({70.f, 100.f});
        menu->setLayout(ColumnLayout::create()->setAxisReverse(true)->setGap(4.f));

        auto add = [&](char const* txt, SEL_MenuHandler cb) {
            auto spr = ButtonSprite::create(txt, 90, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
            menu->addChild(btn);
        };
        add("Grabar", menu_selector(FMPause::onFMRecord));
        add("Parar", menu_selector(FMPause::onFMStop));
        add("Reproducir", menu_selector(FMPause::onFMPlay));
        add("Reset", menu_selector(FMPause::onFMReset));
        menu->updateLayout();
        this->addChild(menu);
    }

    void onFMRecord(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_isPracticeMode) {
            FLAlertLayer::create("Frame Margin", "Activa el modo practica primero.", "OK")->show();
            return;
        }
        S.macro.clear();
        S.recording = true;
        S.playing = false;
        Notification::create("Grabando... reinicia y jugá", NotificationIcon::Success)->show();
    }

    void onFMStop(CCObject*) {
        S.recording = false;
        S.playing = false;
        Notification::create(fmt::format("{} inputs guardados", S.macro.size()), NotificationIcon::Info)->show();
    }

    void onFMPlay(CCObject*) {
        if (S.macro.empty()) {
            Notification::create("No hay macro grabada", NotificationIcon::Warning)->show();
            return;
        }
        S.recording = false;
        S.playing = true;
        S.playIdx = 0;
        Notification::create("Reproduciendo: reinicia el nivel", NotificationIcon::Success)->show();
    }

    void onFMReset(CCObject*) {
        S.counts.fill(0);
        for (int w = 1; w <= MAX_W; ++w) {
            if (S.labels[w]) {
                S.labels[w]->setString(fmt::format("{}: 0", w).c_str());
            }
        }
        Notification::create("Contadores en 0", NotificationIcon::Info)->show();
    }
};
