#pragma once

namespace uxplay_control {

enum class StopState {
    None,
    Mirror,
    PinPrompt,
};

// Product-facing cast events are allowed only after the current RAOP client
// has passed admission. Explicit stop invalidates that admission even while
// the underlying connection is still draining, so late media cannot announce
// a new session. Callers serialize these transitions with control_state_mutex.
struct SessionState {
    unsigned int openConnections = 0;
    bool mirrorStartedAnnounced = false;
    bool pinPromptAnnounced = false;
    bool clientAdmitted = false;

    bool ConnectionOpened()
    {
        ++openConnections;
        return openConnections == 1;
    }

    bool ConnectionClosed()
    {
        if (openConnections > 0) {
            --openConnections;
        }
        if (openConnections != 0) {
            return false;
        }

        const bool stopped = mirrorStartedAnnounced;
        mirrorStartedAnnounced = false;
        clientAdmitted = false;
        if (stopped) {
            pinPromptAnnounced = false;
        }
        return stopped;
    }

    void NoteClientAdmitted()
    {
        clientAdmitted = true;
        pinPromptAnnounced = false;
    }

    bool CanAnnounceMediaStart() const
    {
        return openConnections > 0 && clientAdmitted && !mirrorStartedAnnounced;
    }

    bool TryAnnounceMediaStart()
    {
        if (!CanAnnounceMediaStart()) {
            return false;
        }
        mirrorStartedAnnounced = true;
        return true;
    }

    void NotePinPrompt()
    {
        pinPromptAnnounced = true;
    }

    StopState TakeStopState()
    {
        StopState state = StopState::None;
        if (mirrorStartedAnnounced) {
            state = StopState::Mirror;
        } else if (pinPromptAnnounced) {
            state = StopState::PinPrompt;
        }
        mirrorStartedAnnounced = false;
        pinPromptAnnounced = false;
        clientAdmitted = false;
        return state;
    }
};

} // namespace uxplay_control
