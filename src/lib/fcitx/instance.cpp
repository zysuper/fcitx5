//
// Copyright (C) 2016~2016 by CSSlayer
// wengxt@gmail.com
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; see the file COPYING. If not,
// see <http://www.gnu.org/licenses/>.
//

#include "instance.h"
#include "addonmanager.h"
#include "config.h"
#include "fcitx-config/iniparser.h"
#include "fcitx-utils/event.h"
#include "fcitx-utils/i18n.h"
#include "fcitx-utils/log.h"
#include "fcitx-utils/standardpath.h"
#include "fcitx-utils/stringutils.h"
#include "fcitx-utils/utf8.h"
#include "focusgroup.h"
#include "globalconfig.h"
#include "inputcontextmanager.h"
#include "inputcontextproperty.h"
#include "inputmethodengine.h"
#include "inputmethodentry.h"
#include "inputmethodmanager.h"
#include "misc_p.h"
#include "userinterfacemanager.h"
#include <fcntl.h>
#include <fmt/format.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

FCITX_DEFINE_LOG_CATEGORY(keyTrace, "keyTrace");

#define FCITX_KEYTRACE() FCITX_LOGC(keyTrace, Debug)

namespace {

void initAsDaemon() {
    pid_t pid;
    if ((pid = fork()) > 0) {
        waitpid(pid, nullptr, 0);
        exit(0);
    }
    setsid();
    auto oldint = signal(SIGINT, SIG_IGN);
    auto oldhup = signal(SIGHUP, SIG_IGN);
    auto oldquit = signal(SIGQUIT, SIG_IGN);
    auto oldpipe = signal(SIGPIPE, SIG_IGN);
    auto oldttou = signal(SIGTTOU, SIG_IGN);
    auto oldttin = signal(SIGTTIN, SIG_IGN);
    auto oldchld = signal(SIGCHLD, SIG_IGN);
    if (fork() > 0) {
        exit(0);
    }
    chdir("/");

    signal(SIGINT, oldint);
    signal(SIGHUP, oldhup);
    signal(SIGQUIT, oldquit);
    signal(SIGPIPE, oldpipe);
    signal(SIGTTOU, oldttou);
    signal(SIGTTIN, oldttin);
    signal(SIGCHLD, oldchld);
}
} // namespace

namespace fcitx {

#define FCITX_DEFINE_XKB_AUTOPTR(TYPE)                                         \
    using TYPE##_autoptr = std::unique_ptr<struct TYPE, decltype(&TYPE##_unref)>

FCITX_DEFINE_XKB_AUTOPTR(xkb_context);
FCITX_DEFINE_XKB_AUTOPTR(xkb_compose_table);
FCITX_DEFINE_XKB_AUTOPTR(xkb_compose_state);
FCITX_DEFINE_XKB_AUTOPTR(xkb_state);
FCITX_DEFINE_XKB_AUTOPTR(xkb_keymap);

class CheckInputMethodChanged;

struct InputState : public InputContextProperty {
    InputState(InstancePrivate *d, InputContext *ic);

    void reset() {
        if (xkbComposeState_) {
            xkb_compose_state_reset(xkbComposeState_.get());
        }
        keyReleased_ = -1;
        keyReleasedIndex_ = -2;
        totallyReleased_ = true;
    }

    void showInputMethodInformation(const std::string &name);

    void hideInputMethodInfo() {
        if (!imInfoTimer_) {
            return;
        }
        imInfoTimer_.reset();
        auto &panel = ic_->inputPanel();
        if (panel.auxDown().size() == 0 && panel.preedit().size() == 0 &&
            panel.clientPreedit().size() == 0 &&
            (!panel.candidateList() || panel.candidateList()->size() == 0) &&
            panel.auxUp().size() == 1 &&
            panel.auxUp().stringAt(0) == lastInfo_) {
            panel.reset();
            ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        }
    }

    xkb_state *customXkbState(bool refresh = false);
    void resetXkbState() {
        lastXkbLayout_.clear();
        xkbState_.reset();
    }

    InstancePrivate *d_ptr;
    InputContext *ic_;
    int keyReleased_ = -1;
    // We use -2 to make sure -2 != -1 (From keyListIndex)
    int keyReleasedIndex_ = -2;
    bool totallyReleased_ = true;
    bool firstTrigger_ = false;

    bool active_;
    CheckInputMethodChanged *imChanged_ = nullptr;
    xkb_compose_state_autoptr xkbComposeState_;
    xkb_state_autoptr xkbState_;
    std::string lastXkbLayout_;

    std::unique_ptr<EventSourceTime> imInfoTimer_;
    std::string lastInfo_;
    std::string lastIM_;

    bool lastIMChangeIsAltTrigger_ = false;
};

class CheckInputMethodChanged {
public:
    CheckInputMethodChanged(InputContext *ic, Instance *instance)
        : instance_(instance), ic_(ic->watch()),
          inputMethod_(instance->inputMethod(ic)),
          reason_(InputMethodSwitchedReason::Other) {
        auto inputState = ic->propertyAs<InputState>("inputState");
        if (!inputState->imChanged_) {
            inputState->imChanged_ = this;
        } else {
            ic_.unwatch();
        }
    }

    void ignore() { ignore_ = true; }

    ~CheckInputMethodChanged() {
        if (!ic_.isValid()) {
            return;
        }
        auto ic = ic_.get();
        auto inputState = ic->propertyAs<InputState>("inputState");
        inputState->imChanged_ = nullptr;
        if (inputMethod_ != instance_->inputMethod(ic) && !ignore_) {
            instance_->postEvent(
                InputContextSwitchInputMethodEvent(reason_, inputMethod_, ic));
        }
    }

    void setReason(InputMethodSwitchedReason reason) { reason_ = reason; }

private:
    Instance *instance_;
    TrackableObjectReference<InputContext> ic_;
    std::string inputMethod_;
    InputMethodSwitchedReason reason_;
    bool ignore_ = false;
};

struct InstanceArgument {
    InstanceArgument() = default;
    FCITX_INLINE_DEFINE_DEFAULT_DTOR_AND_COPY(InstanceArgument)

    void parseOption(int argc, char *argv[]);
    void printVersion() { std::cout << FCITX_VERSION_STRING << std::endl; }
    void printUsage() {}

    int overrideDelay = -1;
    bool tryReplace = false;
    bool quietQuit = false;
    bool runAsDaemon = false;
    bool quitWhenMainDisplayDisconnected = true;
    std::string uiName;
    std::vector<std::string> enableList;
    std::vector<std::string> disableList;
};

class InstancePrivate : public QPtrHolder<Instance> {
public:
    InstancePrivate(Instance *q)
        : QPtrHolder<Instance>(q), xkbContext_(nullptr, &xkb_context_unref),
          xkbComposeTable_(nullptr, &xkb_compose_table_unref) {
        const char *locale = getenv("LC_ALL");
        if (!locale) {
            locale = getenv("LC_CTYPE");
        }
        if (!locale) {
            locale = getenv("LANG");
        }
        if (!locale) {
            locale = "C";
        }
        xkbContext_.reset(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
        if (xkbContext_) {
            xkb_context_set_log_level(xkbContext_.get(),
                                      XKB_LOG_LEVEL_CRITICAL);
            xkbComposeTable_.reset(xkb_compose_table_new_from_locale(
                xkbContext_.get(), locale, XKB_COMPOSE_COMPILE_NO_FLAGS));
        }
    }

    std::unique_ptr<HandlerTableEntry<EventHandler>>
    watchEvent(EventType type, EventWatcherPhase phase, EventHandler callback) {
        return eventHandlers_[type][phase].add(callback);
    }

    xkb_keymap *keymap(const std::string &display, const std::string &layout,
                       const std::string &variant) {
        auto layoutAndVariant = stringutils::concat(layout, "-", variant);
        if (auto keymapPtr =
                findValue(keymapCache_[display], layoutAndVariant)) {
            return (*keymapPtr).get();
        }
        struct xkb_rule_names names;
        names.layout = layout.c_str();
        names.variant = variant.c_str();
        std::tuple<std::string, std::string, std::string> xkbParam;
        if (auto param = findValue(xkbParams_, display)) {
            xkbParam = *param;
        } else {
            xkbParam = std::make_tuple(DEFAULT_XKB_RULES, "pc101", "");
        }
        names.rules = std::get<0>(xkbParam).c_str();
        names.model = std::get<1>(xkbParam).c_str();
        names.options = std::get<2>(xkbParam).c_str();
        xkb_keymap_autoptr keymap(
            xkb_keymap_new_from_names(xkbContext_.get(), &names,
                                      XKB_KEYMAP_COMPILE_NO_FLAGS),
            &xkb_keymap_unref);
        auto result =
            keymapCache_[display].emplace(layoutAndVariant, std::move(keymap));
        assert(result.second);
        return result.first->second.get();
    }
    std::pair<std::unordered_set<std::string>, std::unordered_set<std::string>>
    overrideAddons() {
        std::unordered_set<std::string> enabled;
        std::unordered_set<std::string> disabled;
        for (auto &addon : globalConfig_.enabledAddons()) {
            enabled.insert(addon);
        }
        for (auto &addon : globalConfig_.disabledAddons()) {
            enabled.erase(addon);
            disabled.insert(addon);
        }
        for (auto &addon : arg_.enableList) {
            disabled.erase(addon);
            enabled.insert(addon);
        }
        for (auto &addon : arg_.disableList) {
            enabled.erase(addon);
            disabled.insert(addon);
        }
        return {enabled, disabled};
    }

    InstanceArgument arg_;

    int signalPipe_ = -1;
    EventLoop eventLoop_;
    std::unique_ptr<EventSourceIO> signalPipeEvent_;
    std::unique_ptr<EventSource> exitEvent_;
    InputContextManager icManager_;
    AddonManager addonManager_;
    InputMethodManager imManager_{&this->addonManager_};
    UserInterfaceManager uiManager_{&this->addonManager_};
    GlobalConfig globalConfig_;
    std::unordered_map<EventType,
                       std::unordered_map<EventWatcherPhase,
                                          HandlerTable<EventHandler>, EnumHash>,
                       EnumHash>
        eventHandlers_;
    std::vector<std::unique_ptr<HandlerTableEntry<EventHandler>>>
        eventWatchers_;
    std::unique_ptr<EventSource> uiUpdateEvent_;

    FCITX_DEFINE_SIGNAL_PRIVATE(Instance, CommitFilter);
    FCITX_DEFINE_SIGNAL_PRIVATE(Instance, OutputFilter);
    FCITX_DEFINE_SIGNAL_PRIVATE(Instance, KeyEventResult);

    FactoryFor<InputState> inputStateFactory{
        [this](InputContext &ic) { return new InputState(this, &ic); }};

    xkb_context_autoptr xkbContext_;
    xkb_compose_table_autoptr xkbComposeTable_;

    std::vector<ScopedConnection> connections_;
    std::unique_ptr<EventSourceTime> imGroupInfoTimer_;
    std::unique_ptr<EventSourceTime> focusInImInfoTimer_;

    std::unordered_map<std::string,
                       std::unordered_map<std::string, xkb_keymap_autoptr>>
        keymapCache_;
    std::unordered_map<std::string, std::tuple<uint32_t, uint32_t, uint32_t>>
        stateMask_;
    std::unordered_map<std::string,
                       std::tuple<std::string, std::string, std::string>>
        xkbParams_;

    bool restart_ = false;
};

InputState::InputState(InstancePrivate *d, InputContext *ic)
    : d_ptr(d), ic_(ic), xkbComposeState_(nullptr, &xkb_compose_state_unref),
      xkbState_(nullptr, &xkb_state_unref) {
    active_ = d->globalConfig_.activeByDefault();
    if (d->xkbComposeTable_) {
        xkbComposeState_.reset(xkb_compose_state_new(
            d->xkbComposeTable_.get(), XKB_COMPOSE_STATE_NO_FLAGS));
    }
}

void InputState::showInputMethodInformation(const std::string &name) {
    ic_->inputPanel().setAuxUp(Text(name));
    ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    lastInfo_ = name;
    imInfoTimer_ = d_ptr->eventLoop_.addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1000000, 0,
        [this](EventSourceTime *, uint64_t) {
            hideInputMethodInfo();
            return true;
        });
}

xkb_state *InputState::customXkbState(bool refresh) {
    auto instance = d_ptr->q_func();
    auto defaultLayout = d_ptr->imManager_.currentGroup().defaultLayout();
    auto im = instance->inputMethod(ic_);
    auto layout = d_ptr->imManager_.currentGroup().layoutFor(im);
    if (layout.empty() && stringutils::startsWith(im, "keyboard-")) {
        layout = im.substr(9);
    }
    if (layout == defaultLayout || layout.empty()) {
        // Use system one.
        xkbState_.reset();
        lastXkbLayout_.clear();
        return nullptr;
    }

    if (layout == lastXkbLayout_ && !refresh) {
        return xkbState_.get();
    }

    lastXkbLayout_ = layout;
    auto layoutAndVariant = parseLayout(layout);
    if (auto keymap = d_ptr->keymap(ic_->display(), layoutAndVariant.first,
                                    layoutAndVariant.second)) {
        xkbState_.reset(xkb_state_new(keymap));
    } else {
        xkbState_.reset();
    }
    return xkbState_.get();
}

Instance::Instance(int argc, char **argv) {
    InstanceArgument arg;
    arg.parseOption(argc, argv);
    if (arg.quietQuit) {
        throw InstanceQuietQuit();
    }

    if (arg.runAsDaemon) {
        initAsDaemon();
    }

    if (arg.overrideDelay > 0) {
        sleep(arg.overrideDelay);
    }

    // we need fork before this
    d_ptr.reset(new InstancePrivate(this));
    FCITX_D();
    d->arg_ = arg;
    d->addonManager_.setInstance(this);
    d->icManager_.setInstance(this);
    d->connections_.emplace_back(
        d->imManager_.connect<InputMethodManager::CurrentGroupAboutToBeChanged>(
            [this, d](const std::string &) {
                d->icManager_.foreachFocused([this](InputContext *ic) {
                    assert(ic->hasFocus());
                    InputContextSwitchInputMethodEvent event(
                        InputMethodSwitchedReason::GroupChange, "", ic);
                    deactivateInputMethod(event);
                    return true;
                });
                postEvent(InputMethodGroupAboutToChangeEvent());
            }));
    d->connections_.emplace_back(
        d->imManager_.connect<InputMethodManager::CurrentGroupChanged>(
            [this, d](const std::string &) {
                d->icManager_.foreachFocused([this](InputContext *ic) {
                    assert(ic->hasFocus());
                    InputContextSwitchInputMethodEvent event(
                        InputMethodSwitchedReason::GroupChange, "", ic);
                    activateInputMethod(event);
                    return true;
                });
                postEvent(InputMethodGroupChangedEvent());
            }));

    d->icManager_.registerProperty("inputState", &d->inputStateFactory);

    d->eventWatchers_.emplace_back(watchEvent(
        EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
        [this, d](Event &event) {
            auto &keyEvent = static_cast<KeyEvent &>(event);
            auto ic = keyEvent.inputContext();
            CheckInputMethodChanged imChangedRAII(ic, this);

            struct {
                const KeyList &list;
                std::function<bool()> check;
                std::function<void(bool)> trigger;
            } keyHandlers[] = {
                {d->globalConfig_.triggerKeys(),
                 [this]() { return canTrigger(); },
                 [this, ic](bool totallyReleased) {
                     return trigger(ic, totallyReleased);
                 }},
                {d->globalConfig_.altTriggerKeys(),
                 [this, ic]() { return canAltTrigger(ic); },
                 [this, ic](bool) { return altTrigger(ic); }},
                {d->globalConfig_.activateKeys(),
                 [this]() { return canTrigger(); },
                 [this, ic](bool) { return activate(ic); }},
                {d->globalConfig_.deactivateKeys(),
                 [this]() { return canTrigger(); },
                 [this, ic](bool) { return deactivate(ic); }},
                {d->globalConfig_.enumerateForwardKeys(),
                 [this]() { return canTrigger(); },
                 [this, ic](bool) { return enumerate(ic, true); }},
                {d->globalConfig_.enumerateBackwardKeys(),
                 [this]() { return canTrigger(); },
                 [this, ic](bool) { return enumerate(ic, false); }},
                {d->globalConfig_.enumerateGroupForwardKeys(),
                 [this]() { return canChangeGroup(); },
                 [this, ic, d](bool) {
                     auto inputState = ic->propertyFor(&d->inputStateFactory);
                     if (inputState->imChanged_) {
                         inputState->imChanged_->ignore();
                     }
                     return enumerateGroup(true);
                 }},
                {d->globalConfig_.enumerateGroupBackwardKeys(),
                 [this]() { return canChangeGroup(); },
                 [this, ic, d](bool) {
                     auto inputState = ic->propertyFor(&d->inputStateFactory);
                     if (inputState->imChanged_) {
                         inputState->imChanged_->ignore();
                     }
                     return enumerateGroup(true);
                 }},
            };

            auto inputState = ic->propertyFor(&d->inputStateFactory);
            int keyReleased = inputState->keyReleased_;
            int keyReleasedIndex = inputState->keyReleasedIndex_;
            // Keep these two values, and reset them in the state
            inputState->keyReleased_ = -1;
            inputState->keyReleasedIndex_ = -2;
            const bool isModifier = keyEvent.origKey().isModifier();
            if (keyEvent.isRelease()) {
                int idx = 0;
                if (keyEvent.origKey().isModifier() &&
                    Key::keySymToStates(keyEvent.origKey().sym()) ==
                        keyEvent.origKey().states()) {
                    inputState->totallyReleased_ = true;
                }
                for (auto &keyHandler : keyHandlers) {
                    if (keyReleased == idx &&
                        keyReleasedIndex ==
                            keyEvent.origKey().keyListIndex(keyHandler.list) &&
                        keyHandler.check()) {
                        if (isModifier) {
                            keyHandler.trigger(inputState->totallyReleased_);
                            if (keyEvent.origKey().hasModifier()) {
                                inputState->totallyReleased_ = false;
                            }
                            return keyEvent.filterAndAccept();
                        } else {
                            return keyEvent.filter();
                        }
                    }
                    idx++;
                }
            }

            if (!keyEvent.filtered() && !keyEvent.isRelease()) {
                int idx = 0;
                for (auto &keyHandler : keyHandlers) {
                    auto keyIdx =
                        keyEvent.origKey().keyListIndex(keyHandler.list);
                    if (keyIdx >= 0 && keyHandler.check()) {
                        inputState->keyReleased_ = idx;
                        inputState->keyReleasedIndex_ = keyIdx;
                        if (isModifier) {
                            // don't forward to input method, but make it pass
                            // through to client.
                            return keyEvent.filter();
                        } else {
                            keyHandler.trigger(inputState->totallyReleased_);
                            if (keyEvent.origKey().hasModifier()) {
                                inputState->totallyReleased_ = false;
                            }
                            return keyEvent.filterAndAccept();
                        }
                    }
                    idx++;
                }
            }
        }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextKeyEvent, EventWatcherPhase::ReservedFirst,
        [d](Event &event) {
            auto &keyEvent = static_cast<KeyEvent &>(event);
            auto ic = keyEvent.inputContext();
            auto inputState = ic->propertyFor(&d->inputStateFactory);
            auto xkbState = inputState->customXkbState();
            FCITX_KEYTRACE() << "KeyEvent: " << keyEvent.key()
                             << " Release:" << keyEvent.isRelease();
            if (xkbState) {
                if (auto mods = findValue(d->stateMask_, ic->display())) {
                    FCITX_LOG(Debug) << "Update mask to customXkbState";
                    // Keep depressed, but propagate latched and locked.
                    auto depressed = xkb_state_serialize_mods(
                        xkbState, XKB_STATE_MODS_DEPRESSED);
                    auto latched = std::get<1>(*mods);
                    auto locked = std::get<2>(*mods);

                    // set modifiers in depressed if they don't appear in any of
                    // the final masks
                    // depressed |= ~(depressed | latched | locked);
                    FCITX_LOG(Debug)
                        << depressed << " " << latched << " " << locked;
                    xkb_state_update_mask(xkbState, depressed, latched, locked,
                                          0, 0, 0);
                }
                FCITX_LOG(Debug) << "XkbState update key";
                xkb_state_update_key(xkbState, keyEvent.rawKey().code(),
                                     keyEvent.isRelease() ? XKB_KEY_UP
                                                          : XKB_KEY_DOWN);

                const uint32_t modsDepressed = xkb_state_serialize_mods(
                    xkbState, XKB_STATE_MODS_DEPRESSED);
                const uint32_t modsLatched =
                    xkb_state_serialize_mods(xkbState, XKB_STATE_MODS_LATCHED);
                const uint32_t modsLocked =
                    xkb_state_serialize_mods(xkbState, XKB_STATE_MODS_LOCKED);
                FCITX_LOG(Debug) << "Current mods" << modsDepressed
                                 << modsLatched << modsLocked;
                auto newSym = xkb_state_key_get_one_sym(
                    xkbState, keyEvent.rawKey().code());
                auto newModifier = keyEvent.rawKey().states();
                auto newCode = keyEvent.rawKey().code();
                Key key(static_cast<KeySym>(newSym), newModifier, newCode);
                FCITX_LOG(Debug)
                    << "Custom Xkb translated Key: " << key.toString();
                keyEvent.setKey(key.normalize());
            }

            if (keyEvent.isRelease()) {
                return;
            }
            inputState->hideInputMethodInfo();
        }));
    d->eventWatchers_.emplace_back(
        watchEvent(EventType::InputContextKeyEvent,
                   EventWatcherPhase::InputMethod, [this](Event &event) {
                       auto &keyEvent = static_cast<KeyEvent &>(event);
                       auto ic = keyEvent.inputContext();
                       auto engine = inputMethodEngine(ic);
                       auto entry = inputMethodEntry(ic);
                       if (!engine || !entry) {
                           return;
                       }
                       engine->keyEvent(*entry, keyEvent);
                   }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextKeyEvent, EventWatcherPhase::ReservedLast,
        [this, d](Event &event) {
            auto &keyEvent = static_cast<KeyEvent &>(event);
            auto ic = keyEvent.inputContext();
            auto engine = inputMethodEngine(ic);
            auto entry = inputMethodEntry(ic);
            if (!engine || !entry) {
                return;
            }
            engine->filterKey(*entry, keyEvent);
            auto inputState = ic->propertyFor(&d->inputStateFactory);
            emit<Instance::KeyEventResult>(keyEvent);
            if (keyEvent.forward()) {
                if (auto xkbState = inputState->customXkbState()) {
                    if (auto utf32 = xkb_state_key_get_utf32(
                            xkbState, keyEvent.key().code())) {
                        // Ignore backspace, return, backspace, and delete.
                        if (utf32 == '\n' || utf32 == '\b' || utf32 == '\r' ||
                            utf32 == '\033' || utf32 == '\x7f') {
                            return;
                        }
                        if (keyEvent.key().states().test(KeyState::Ctrl) ||
                            keyEvent.key().sym() == keyEvent.origKey().sym()) {
                            return;
                        }
                        if (!keyEvent.isRelease()) {
                            FCITX_LOG(Debug) << "Will commit char: " << utf32;
                            ic->commitString(utf8::UCS4ToUTF8(utf32));
                        }
                        keyEvent.filterAndAccept();
                    }
                }
            }
        }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextFocusIn, EventWatcherPhase::ReservedFirst,
        [this, d](Event &event) {
            auto &icEvent = static_cast<InputContextEvent &>(event);
            activateInputMethod(icEvent);
            if (!d->globalConfig_.showInputMethodInformationWhenFocusIn()) {
                return;
            }
            // Give some time because the cursor location may need some time
            // to be updated.
            d->focusInImInfoTimer_ = d->eventLoop_.addTimeEvent(
                CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 30000, 0,
                [this, icRef = icEvent.inputContext()->watch()](
                    EventSourceTime *, uint64_t) {
                    // Check if ic is still valid and has focus.
                    if (auto ic = icRef.get(); ic && ic->hasFocus()) {
                        showInputMethodInformation(ic);
                    }
                    return true;
                });
        }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextFocusOut, EventWatcherPhase::ReservedFirst,
        [this, d](Event &event) {
            auto &icEvent = static_cast<InputContextEvent &>(event);
            auto ic = icEvent.inputContext();
            auto inputState = ic->propertyFor(&d->inputStateFactory);
            inputState->reset();
            if (!ic->capabilityFlags().test(
                    CapabilityFlag::ClientUnfocusCommit)) {
                // do server side commit
                auto commit =
                    ic->inputPanel().clientPreedit().toStringForCommit();
                if (commit.size()) {
                    ic->commitString(commit);
                }
            }
            deactivateInputMethod(icEvent);
            ic->statusArea().clear();
        }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextReset, EventWatcherPhase::ReservedFirst,
        [d](Event &event) {
            auto &icEvent = static_cast<InputContextEvent &>(event);
            auto ic = icEvent.inputContext();
            auto inputState = ic->propertyFor(&d->inputStateFactory);
            inputState->reset();
        }));
    d->eventWatchers_.emplace_back(
        watchEvent(EventType::InputContextReset, EventWatcherPhase::InputMethod,
                   [this](Event &event) {
                       auto &icEvent = static_cast<InputContextEvent &>(event);
                       auto ic = icEvent.inputContext();
                       if (!ic->hasFocus()) {
                           return;
                       }
                       auto engine = inputMethodEngine(ic);
                       auto entry = inputMethodEntry(ic);
                       if (!engine || !entry) {
                           return;
                       }
                       engine->reset(*entry, icEvent);
                   }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextSwitchInputMethod,
        EventWatcherPhase::ReservedFirst, [this, d](Event &event) {
            auto &icEvent =
                static_cast<InputContextSwitchInputMethodEvent &>(event);
            auto ic = icEvent.inputContext();
            if (!ic->hasFocus()) {
                return;
            }
            if (auto oldEntry = d->imManager_.entry(icEvent.oldInputMethod())) {
                auto inputState = ic->propertyFor(&d->inputStateFactory);
                FCITX_LOG(Debug) << "Deactivate: "
                                 << "[Last]:" << inputState->lastIM_
                                 << " [Activating]:" << oldEntry->uniqueName();
                assert(inputState->lastIM_ == oldEntry->uniqueName());
                inputState->lastIM_.clear();
                auto oldEngine = static_cast<InputMethodEngine *>(
                    d->addonManager_.addon(oldEntry->addon()));
                if (oldEngine) {
                    oldEngine->deactivate(*oldEntry, icEvent);
                    postEvent(InputMethodDeactivatedEvent(
                        oldEntry->uniqueName(), ic));
                }
            }

            activateInputMethod(icEvent);
        }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextSwitchInputMethod,
        EventWatcherPhase::ReservedLast, [this, d](Event &event) {
            auto &icEvent =
                static_cast<InputContextSwitchInputMethodEvent &>(event);
            auto ic = icEvent.inputContext();
            if (!ic->hasFocus()) {
                return;
            }

            auto inputState = ic->propertyFor(&d->inputStateFactory);
            inputState->lastIMChangeIsAltTrigger_ =
                icEvent.reason() == InputMethodSwitchedReason::AltTrigger;

            if ((icEvent.reason() != InputMethodSwitchedReason::Trigger &&
                 icEvent.reason() != InputMethodSwitchedReason::AltTrigger &&
                 icEvent.reason() != InputMethodSwitchedReason::Enumerate &&
                 icEvent.reason() != InputMethodSwitchedReason::Activate &&
                 icEvent.reason() != InputMethodSwitchedReason::Other &&
                 icEvent.reason() != InputMethodSwitchedReason::GroupChange &&
                 icEvent.reason() != InputMethodSwitchedReason::Deactivate)) {
                return;
            }
            showInputMethodInformation(ic);
        }));
    d->eventWatchers_.emplace_back(
        d->watchEvent(EventType::InputMethodGroupChanged,
                      EventWatcherPhase::ReservedLast, [this, d](Event &) {
                          // Use a timer here. so we can get focus back to real
                          // window.
                          d->imGroupInfoTimer_ = d->eventLoop_.addTimeEvent(
                              CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 30000, 0,
                              [this](EventSourceTime *, uint64_t) {
                                  inputContextManager().foreachFocused(
                                      [this](InputContext *ic) {
                                          showInputMethodInformation(ic);
                                          return true;
                                      });
                                  return true;
                              });
                      }));

    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextUpdateUI, EventWatcherPhase::ReservedFirst,
        [d](Event &event) {
            auto &icEvent = static_cast<InputContextUpdateUIEvent &>(event);
            if (icEvent.immediate()) {
                d->uiManager_.update(icEvent.component(),
                                     icEvent.inputContext());
                d->uiManager_.flush();
            } else {
                d->uiManager_.update(icEvent.component(),
                                     icEvent.inputContext());
                d->uiUpdateEvent_->setOneShot();
            }
        }));
    d->eventWatchers_.emplace_back(d->watchEvent(
        EventType::InputContextDestroyed, EventWatcherPhase::ReservedFirst,
        [d](Event &event) {
            auto &icEvent = static_cast<InputContextEvent &>(event);
            d->uiManager_.expire(icEvent.inputContext());
        }));
    d->uiUpdateEvent_ = d->eventLoop_.addDeferEvent([d](EventSource *) {
        d->uiManager_.flush();
        return true;
    });
    d->uiUpdateEvent_->setEnabled(false);
}

Instance::~Instance() {
    FCITX_D();
    d->icManager_.finalize();
    d->addonManager_.unload();
    d->icManager_.setInstance(nullptr);
}

void InstanceArgument::parseOption(int argc, char **argv) {
    struct option longOptions[] = {{"enable", required_argument, nullptr, 0},
                                   {"disable", required_argument, nullptr, 0},
                                   {"verbose", required_argument, nullptr, 0},
                                   {"keep", no_argument, nullptr, 'k'},
                                   {"ui", required_argument, nullptr, 'u'},
                                   {"replace", no_argument, nullptr, 'r'},
                                   {"version", no_argument, nullptr, 'v'},
                                   {"help", no_argument, nullptr, 'h'},
                                   {nullptr, 0, 0, 0}};

    int optionIndex = 0;
    int c;
    while ((c = getopt_long(argc, argv, "ru:dDs:hv", longOptions,
                            &optionIndex)) != EOF) {
        switch (c) {
        case 0: {
            switch (optionIndex) {
            case 0:
                enableList = stringutils::split(optarg, ",");
                break;
            case 1:
                disableList = stringutils::split(optarg, ",");
                break;
            case 2:
                Log::setLogRule(optarg);
                break;
            default:
                quietQuit = true;
                printUsage();
                break;
            }
        } break;
        case 'r':
            tryReplace = true;
            break;
        case 'u':
            uiName = optarg;
            break;
        case 'd':
            runAsDaemon = true;
            break;
        case 'D':
            runAsDaemon = false;
            break;
        case 'k':
            quitWhenMainDisplayDisconnected = false;
            break;
        case 's':
            overrideDelay = std::atoi(optarg);
            break;
        case 'h':
            quietQuit = true;
            printUsage();
            break;
        case 'v':
            quietQuit = true;
            printVersion();
            break;
        default:
            quietQuit = true;
            printUsage();
        }
    }
}

void Instance::setSignalPipe(int fd) {
    FCITX_D();
    d->signalPipe_ = fd;
    d->signalPipeEvent_ = d->eventLoop_.addIOEvent(
        fd, IOEventFlag::In, [this](EventSource *, int, IOEventFlags) {
            handleSignal();
            return true;
        });
}

bool Instance::willTryReplace() const {
    FCITX_D();
    return d->arg_.tryReplace;
}

bool Instance::quitWhenMainDisplayDisconnected() const {
    FCITX_D();
    return d->arg_.quitWhenMainDisplayDisconnected;
}

void Instance::handleSignal() {
    FCITX_D();
    uint8_t signo = 0;
    while (fs::safeRead(d->signalPipe_, &signo, sizeof(signo)) > 0) {
        if (signo == SIGINT || signo == SIGTERM || signo == SIGQUIT ||
            signo == SIGXCPU) {
            exit();
        } else if (signo == SIGUSR1) {
            reloadConfig();
        }
    }
}

void Instance::initialize() {
    FCITX_D();
    if (d->arg_.uiName.size()) {
        d->arg_.enableList.push_back(d->arg_.uiName);
    }
    reloadConfig();
    std::unordered_set<std::string> enabled;
    std::unordered_set<std::string> disabled;
    std::tie(enabled, disabled) = d->overrideAddons();
    FCITX_INFO() << "Override Enabled Addons: " << enabled;
    FCITX_INFO() << "Override Disabled Addons: " << disabled;
    d->addonManager_.load(enabled, disabled);
    d->imManager_.load();
    d->uiManager_.load(d->arg_.uiName);
    d->exitEvent_ = d->eventLoop_.addExitEvent([this](EventSource *) {
        FCITX_LOG(Debug) << "Running save...";
        FCITX_D();
        save();
        if (d->restart_) {
            auto fcitxBinary = StandardPath::fcitxPath("bindir", "fcitx5");
            std::vector<char> command{fcitxBinary.begin(), fcitxBinary.end()};
            command.push_back('\0');
            char *const argv[] = {command.data(), nullptr};
            execv(argv[0], argv);
            perror("Restart failed: execvp:");
            _exit(1);
        }
        return false;
    });
}

int Instance::exec() {
    FCITX_D();
    if (d->arg_.quietQuit) {
        return 0;
    }
    initialize();
    auto r = eventLoop().exec();

    return r ? 0 : 1;
}

EventLoop &Instance::eventLoop() {
    FCITX_D();
    return d->eventLoop_;
}

InputContextManager &Instance::inputContextManager() {
    FCITX_D();
    return d->icManager_;
}

AddonManager &Instance::addonManager() {
    FCITX_D();
    return d->addonManager_;
}

InputMethodManager &Instance::inputMethodManager() {
    FCITX_D();
    return d->imManager_;
}

const InputMethodManager &Instance::inputMethodManager() const {
    FCITX_D();
    return d->imManager_;
}

UserInterfaceManager &Instance::userInterfaceManager() {
    FCITX_D();
    return d->uiManager_;
}

GlobalConfig &Instance::globalConfig() {
    FCITX_D();
    return d->globalConfig_;
}

bool Instance::postEvent(Event &event) {
    FCITX_D();
    auto iter = d->eventHandlers_.find(event.type());
    if (iter != d->eventHandlers_.end()) {
        auto &handlers = iter->second;
        EventWatcherPhase phaseOrder[] = {
            EventWatcherPhase::ReservedFirst, EventWatcherPhase::PreInputMethod,
            EventWatcherPhase::InputMethod, EventWatcherPhase::PostInputMethod,
            EventWatcherPhase::ReservedLast};

        for (auto phase : phaseOrder) {
            auto iter2 = handlers.find(phase);
            if (iter2 != handlers.end()) {
                for (auto &handler : iter2->second.view()) {
                    handler(event);
                    if (event.filtered()) {
                        return event.accepted();
                    }
                }
            }
        }
    }
    return event.accepted();
}

std::unique_ptr<HandlerTableEntry<EventHandler>>
Instance::watchEvent(EventType type, EventWatcherPhase phase,
                     EventHandler callback) {
    FCITX_D();
    if (phase == EventWatcherPhase::ReservedFirst ||
        phase == EventWatcherPhase::ReservedLast) {
        throw std::invalid_argument("Reserved Phase is only for internal use");
    }
    return d->watchEvent(type, phase, callback);
}

std::string Instance::inputMethod(InputContext *ic) {
    FCITX_D();
    auto &group = d->imManager_.currentGroup();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    if (group.inputMethodList().empty()) {
        return "";
    }
    if (inputState->active_) {
        return group.defaultInputMethod();
    }

    return group.inputMethodList()[0].name();
}

const InputMethodEntry *Instance::inputMethodEntry(InputContext *ic) {
    FCITX_D();
    auto imName = inputMethod(ic);
    if (imName.empty()) {
        return nullptr;
    }
    return d->imManager_.entry(imName);
}

InputMethodEngine *Instance::inputMethodEngine(InputContext *ic) {
    FCITX_D();
    auto entry = inputMethodEntry(ic);
    if (!entry) {
        return nullptr;
    }
    return static_cast<InputMethodEngine *>(
        d->addonManager_.addon(entry->addon(), true));
}

InputMethodEngine *Instance::inputMethodEngine(const std::string &name) {
    FCITX_D();
    auto entry = d->imManager_.entry(name);
    if (!entry) {
        return nullptr;
    }
    return static_cast<InputMethodEngine *>(
        d->addonManager_.addon(entry->addon(), true));
}

uint32_t Instance::processCompose(InputContext *ic, KeySym keysym) {
    FCITX_D();
    auto state = ic->propertyFor(&d->inputStateFactory);

    if (!state->xkbComposeState_) {
        return 0;
    }

    auto keyval = static_cast<xkb_keysym_t>(keysym);

    enum xkb_compose_feed_result result =
        xkb_compose_state_feed(state->xkbComposeState_.get(), keyval);
    if (result == XKB_COMPOSE_FEED_IGNORED) {
        return 0;
    }

    enum xkb_compose_status status =
        xkb_compose_state_get_status(state->xkbComposeState_.get());
    if (status == XKB_COMPOSE_NOTHING) {
        return 0;
    } else if (status == XKB_COMPOSE_COMPOSED) {
        char buffer[FCITX_UTF8_MAX_LENGTH + 1] = {'\0', '\0', '\0', '\0',
                                                  '\0', '\0', '\0'};
        int length = xkb_compose_state_get_utf8(state->xkbComposeState_.get(),
                                                buffer, sizeof(buffer));
        xkb_compose_state_reset(state->xkbComposeState_.get());
        if (length == 0) {
            return FCITX_INVALID_COMPOSE_RESULT;
        }

        uint32_t c = 0;
        fcitx_utf8_get_char(buffer, &c);
        return c;
    } else if (status == XKB_COMPOSE_CANCELLED) {
        xkb_compose_state_reset(state->xkbComposeState_.get());
    }

    return FCITX_INVALID_COMPOSE_RESULT;
}

void Instance::resetCompose(InputContext *inputContext) {
    FCITX_D();
    auto state = inputContext->propertyFor(&d->inputStateFactory);

    if (!state->xkbComposeState_) {
        return;
    }
    xkb_compose_state_reset(state->xkbComposeState_.get());
}

void Instance::save() {
    FCITX_D();
    d->imManager_.save();
    d->addonManager_.saveAll();
}

void Instance::activate() {
    if (auto ic = lastFocusedInputContext()) {
        CheckInputMethodChanged imChangedRAII(ic, this);
        activate(ic);
    }
}

std::string Instance::addonForInputMethod(const std::string &imName) {

    if (auto entry = inputMethodManager().entry(imName)) {
        return entry->uniqueName();
    }
    return {};
}

void Instance::configure() {}

void Instance::configureAddon(const std::string &) {}

void Instance::configureInputMethod(const std::string &imName) {
    auto addon = addonForInputMethod(imName);
    if (!addon.empty()) {
        return configureAddon(addon);
    }
}

std::string Instance::currentInputMethod() {
    if (auto ic = lastFocusedInputContext()) {
        if (auto entry = inputMethodEntry(ic)) {
            return entry->uniqueName();
        }
    }
    return {};
}

std::string Instance::currentUI() {
    FCITX_D();
    return d->uiManager_.currentUI();
}

void Instance::deactivate() {
    if (auto ic = lastFocusedInputContext()) {
        CheckInputMethodChanged imChangedRAII(ic, this);
        deactivate(ic);
    }
}

void Instance::exit() { eventLoop().quit(); }

void Instance::reloadAddonConfig(const std::string &addonName) {
    auto addon = addonManager().addon(addonName);
    if (addon) {
        addon->reloadConfig();
    }
}

void Instance::reloadConfig() {
    FCITX_D();
    auto &standardPath = StandardPath::global();
    auto file =
        standardPath.open(StandardPath::Type::PkgConfig, "config", O_RDONLY);
    RawConfig config;
    readFromIni(config, file.fd());
    d->globalConfig_.load(config);
    FCITX_LOG(Debug) << "Trigger Key: "
                     << Key::keyListToString(d->globalConfig_.triggerKeys());
}

void Instance::resetInputMethodList() {}

void Instance::restart() {
    FCITX_D();
    d->restart_ = true;
    exit();
}

void Instance::setCurrentInputMethod(const std::string &name) {
    FCITX_D();
    if (!canTrigger()) {
        return;
    }
    if (auto ic = lastFocusedInputContext()) {
        CheckInputMethodChanged imChangedRAII(ic, this);
        auto currentIM = inputMethod(ic);
        if (currentIM == name) {
            return;
        }
        auto &imManager = inputMethodManager();
        auto inputState = ic->propertyFor(&d->inputStateFactory);
        const auto &imList = imManager.currentGroup().inputMethodList();

        auto iter = std::find_if(imList.begin(), imList.end(),
                                 [&name](const InputMethodGroupItem &item) {
                                     return item.name() == name;
                                 });
        if (iter == imList.end()) {
            return;
        }
        auto idx = std::distance(imList.begin(), iter);
        if (idx != 0) {
            imManager.currentGroup().setDefaultInputMethod(name);
            inputState->active_ = true;
        } else {
            inputState->active_ = false;
        }
        if (inputState->imChanged_) {
            inputState->imChanged_->setReason(InputMethodSwitchedReason::Other);
        }
    }
}

int Instance::state() {
    FCITX_D();
    if (auto ic = lastFocusedInputContext()) {
        auto inputState = ic->propertyFor(&d->inputStateFactory);
        return inputState->active_ ? 2 : 1;
    }
    return 0;
}

void Instance::toggle() {
    if (auto ic = lastFocusedInputContext()) {
        CheckInputMethodChanged imChangedRAII(ic, this);
        trigger(ic, true);
    }
}

void Instance::enumerate(bool forward) {
    if (auto ic = lastFocusedInputContext()) {
        CheckInputMethodChanged imChangedRAII(ic, this);
        enumerate(ic, forward);
    }
}

bool Instance::canTrigger() const {
    auto &imManager = inputMethodManager();
    return (imManager.currentGroup().inputMethodList().size() > 1);
}

bool Instance::canAltTrigger(InputContext *ic) const {
    if (!canTrigger()) {
        return false;
    }
    FCITX_D();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    if (inputState->active_) {
        return true;
    }
    return inputState->lastIMChangeIsAltTrigger_;
}

bool Instance::canChangeGroup() const {
    auto &imManager = inputMethodManager();
    return (imManager.groupCount() > 1);
}

bool Instance::toggle(InputContext *ic, InputMethodSwitchedReason reason) {
    FCITX_D();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    if (!canTrigger()) {
        return false;
    }
    inputState->active_ = !inputState->active_;
    if (inputState->imChanged_) {
        inputState->imChanged_->setReason(reason);
    }
    return true;
}

bool Instance::trigger(InputContext *ic, bool totallyReleased) {
    FCITX_D();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    if (!canTrigger()) {
        return false;
    }
    // Active -> inactive -> enumerate.
    // Inactive -> active -> inactive -> enumerate.
    if (totallyReleased) {
        toggle(ic);
        inputState->firstTrigger_ = true;
    } else {
        if (inputState->firstTrigger_ && inputState->active_) {
            toggle(ic);
        } else {
            enumerate(ic, true);
        }
        inputState->firstTrigger_ = false;
    }
    return true;
}

bool Instance::altTrigger(InputContext *ic) {
    if (!canAltTrigger(ic)) {
        return false;
    }

    toggle(ic, InputMethodSwitchedReason::AltTrigger);
    return true;
}

bool Instance::activate(InputContext *ic) {
    FCITX_D();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    if (!canTrigger()) {
        return false;
    }
    if (inputState->active_) {
        return true;
    }
    inputState->active_ = true;
    if (inputState->imChanged_) {
        inputState->imChanged_->setReason(InputMethodSwitchedReason::Activate);
    }
    return true;
}

bool Instance::deactivate(InputContext *ic) {
    FCITX_D();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    if (!canTrigger()) {
        return false;
    }
    if (!inputState->active_) {
        return true;
    }
    inputState->active_ = false;
    if (inputState->imChanged_) {
        inputState->imChanged_->setReason(
            InputMethodSwitchedReason::Deactivate);
    }
    return true;
}

bool Instance::enumerate(InputContext *ic, bool forward) {
    FCITX_D();
    auto &imManager = inputMethodManager();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    const auto &imList = imManager.currentGroup().inputMethodList();
    if (!canTrigger()) {
        return false;
    }

    auto currentIM = inputMethod(ic);

    auto iter = std::find_if(imList.begin(), imList.end(),
                             [&currentIM](const InputMethodGroupItem &item) {
                                 return item.name() == currentIM;
                             });
    if (iter == imList.end()) {
        return false;
    }
    auto idx = std::distance(imList.begin(), iter);
    // be careful not to use negative to avoid overflow.
    idx = (idx + (forward ? 1 : (imList.size() - 1))) % imList.size();
    if (idx != 0) {
        imManager.currentGroup().setDefaultInputMethod(imList[idx].name());
        inputState->active_ = true;
    } else {
        inputState->active_ = false;
    }
    if (inputState->imChanged_) {
        inputState->imChanged_->setReason(InputMethodSwitchedReason::Enumerate);
    }

    return true;
}

std::string Instance::commitFilter(InputContext *inputContext,
                                   const std::string &orig) {
    std::string result = orig;
    emit<Instance::CommitFilter>(inputContext, result);
    return result;
}

Text Instance::outputFilter(InputContext *inputContext, const Text &orig) {
    Text result = orig;
    emit<Instance::OutputFilter>(inputContext, result);
    if ((&orig == &inputContext->inputPanel().clientPreedit() ||
         &orig == &inputContext->inputPanel().preedit()) &&
        inputContext->capabilityFlags().test(CapabilityFlag::Password)) {
        Text newText;
        for (int i = 0, e = result.size(); i < e; i++) {
            auto length = utf8::length(result.stringAt(i));
            std::string dot;
            dot.reserve(length * 3);
            while (length != 0) {
                dot += "\xe2\x80\xa2";
                length -= 1;
            }
            newText.append(dot,
                           result.formatAt(i) | TextFormatFlag::DontCommit);
        }
        result = std::move(newText);
    }
    return result;
}

InputContext *Instance::lastFocusedInputContext() {
    FCITX_D();
    return d->icManager_.lastFocusedInputContext();
}

InputContext *Instance::mostRecentInputContext() {
    FCITX_D();
    return d->icManager_.mostRecentInputContext();
}

void Instance::flushUI() {
    FCITX_D();
    d->uiManager_.flush();
}

int scoreForGroup(FocusGroup *group, const std::string &displayHint) {
    // Hardcode wayland over X11.
    if (displayHint.empty()) {
        if (group->display() == "x11:") {
            return 2;
        }
        if (stringutils::startsWith(group->display(), "x11:")) {
            return 1;
        }
        if (group->display() == "wayland:") {
            return 4;
        }
        if (stringutils::startsWith(group->display(), "wayland:")) {
            return 3;
        }
    } else {
        if (group->display() == displayHint) {
            return 2;
        }
        if (stringutils::startsWith(group->display(), displayHint)) {
            return 1;
        }
    }
    return -1;
}

FocusGroup *Instance::defaultFocusGroup(const std::string &displayHint) {
    FCITX_D();
    FocusGroup *defaultFocusGroup = nullptr;

    int score = 0;
    d->icManager_.foreachGroup(
        [&score, &displayHint, &defaultFocusGroup](FocusGroup *group) {
            auto newScore = scoreForGroup(group, displayHint);
            if (newScore > score) {
                defaultFocusGroup = group;
                score = newScore;
            }

            return true;
        });
    return defaultFocusGroup;
}

void Instance::activateInputMethod(InputContextEvent &event) {
    FCITX_D();
    InputContext *ic = event.inputContext();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    auto entry = inputMethodEntry(ic);
    if (entry) {
        FCITX_LOG(Debug) << "Activate: "
                         << "[Last]:" << inputState->lastIM_
                         << " [Activating]:" << entry->uniqueName();
        assert(inputState->lastIM_.empty());
        inputState->lastIM_ = entry->uniqueName();
    }
    auto engine = inputMethodEngine(ic);
    if (!engine || !entry) {
        return;
    }
    if (auto xkbState = inputState->customXkbState(true)) {
        if (auto mods = findValue(d->stateMask_, ic->display())) {
            FCITX_LOG(Debug) << "Update mask to customXkbState";
            auto depressed = std::get<0>(*mods);
            auto latched = std::get<1>(*mods);
            auto locked = std::get<2>(*mods);

            // set modifiers in depressed if they don't appear in any of the
            // final masks
            // depressed |= ~(depressed | latched | locked);
            FCITX_LOG(Debug) << depressed << " " << latched << " " << locked;
            xkb_state_update_mask(xkbState, 0, latched, locked, 0, 0, 0);
        }
    }
    engine->activate(*entry, event);
    postEvent(InputMethodActivatedEvent(entry->uniqueName(), ic));
}

void Instance::deactivateInputMethod(InputContextEvent &event) {
    FCITX_D();
    InputContext *ic = event.inputContext();
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    auto entry = inputMethodEntry(ic);
    if (entry) {
        FCITX_LOG(Debug) << "Deactivate: "
                         << "[Last]:" << inputState->lastIM_
                         << " [Deactivating]:" << entry->uniqueName();
        assert(entry->uniqueName() == inputState->lastIM_);
    }
    inputState->lastIM_.clear();
    auto engine = inputMethodEngine(ic);
    if (!engine || !entry) {
        return;
    }
    engine->deactivate(*entry, event);
    postEvent(InputMethodDeactivatedEvent(entry->uniqueName(), ic));
}

bool Instance::enumerateGroup(bool forward) {
    auto &imManager = inputMethodManager();
    auto groups = imManager.groups();
    if (groups.size() <= 1) {
        return false;
    }
    if (forward) {
        imManager.setCurrentGroup(groups[1]);
    } else {
        imManager.setCurrentGroup(groups.back());
    }
    return true;
}

void Instance::showInputMethodInformation(InputContext *ic) {
    FCITX_LOG(Debug) << "Input method switched";
    FCITX_D();
    if (!d->globalConfig_.showInputMethodInformation()) {
        return;
    }
    auto inputState = ic->propertyFor(&d->inputStateFactory);
    auto engine = inputMethodEngine(ic);
    auto entry = inputMethodEntry(ic);
    auto &imManager = inputMethodManager();
    std::string display;
    if (engine) {
        auto subMode = engine->subMode(*entry, *ic);
        if (subMode.empty()) {
            display = entry->name();
        } else {
            display = fmt::format(_("{0} ({1})"), entry->name(), subMode);
        }
    } else if (entry) {
        display = fmt::format(_("{0} (Not available)"), entry->name());
    } else {
        display = _("(Not available)");
    }
    if (imManager.groupCount() > 1) {
        display = fmt::format(_("Group {0}: {1}"),
                              imManager.currentGroup().name(), display);
    }
    inputState->showInputMethodInformation(display);
}

void Instance::setXkbParameters(const std::string &display,
                                const std::string &rule,
                                const std::string &model,
                                const std::string &options) {
    FCITX_D();
    bool resetState = false;
    if (auto param = findValue(d->xkbParams_, display)) {
        if (std::get<0>(*param) != rule || std::get<1>(*param) != model ||
            std::get<2>(*param) != options) {
            std::get<0>(*param) = rule;
            std::get<1>(*param) = model;
            std::get<2>(*param) = options;
            resetState = true;
        }
    } else {
        d->xkbParams_.emplace(display, std::make_tuple(rule, model, options));
    }

    if (resetState) {
        d->keymapCache_[display].clear();
        d->icManager_.foreach([d, &display](InputContext *ic) {
            if (ic->display() == display) {
                auto inputState = ic->propertyFor(&d->inputStateFactory);
                inputState->resetXkbState();
            }
            return true;
        });
    }
}

void Instance::updateXkbStateMask(const std::string &display,
                                  uint32_t depressed_mods,
                                  uint32_t latched_mods, uint32_t locked_mods) {
    FCITX_D();
    d->stateMask_[display] =
        std::make_tuple(depressed_mods, latched_mods, locked_mods);
}
} // namespace fcitx
