// guids.h -- our COM identities.

#pragma once

#include <guiddef.h>

// {5E52BF50-F229-46A0-8B7D-AA805D47FB60} -- MiniEQ APO (SFX)
DEFINE_GUID(CLSID_MiniEQAPO,
    0x5e52bf50, 0xf229, 0x46a0, 0x8b, 0x7d, 0xaa, 0x80, 0x5d, 0x47, 0xfb, 0x60);

// {767FDA4C-9D15-430F-B142-FAF8F509A793} -- property-store context for our UI.
// Passed as the activation param when the UI opens IAudioSystemEffectsPropertyStore.
DEFINE_GUID(GUID_MiniEQPropStoreCtx,
    0x767fda4c, 0x9d15, 0x430f, 0xb1, 0x42, 0xfa, 0xf8, 0xf5, 0x09, 0xa7, 0x93);

// {97C10020-1218-40F0-9730-F4E4F50428E4} -- the "MiniEQ" system effect.
// Advertised via IAudioSystemEffects::GetEffectsList and
// IAudioSystemEffects2::GetControllableSystemEffectsList so Windows' own
// audio-enhancements UI lists MiniEQ as a toggleable enhancement; toggled
// via IAudioSystemEffects3::SetAudioSystemEffectState.
DEFINE_GUID(GUID_MiniEQEffect,
    0x97c10020, 0x1218, 0x40f0, 0x97, 0x30, 0xf4, 0xe4, 0xf5, 0x04, 0x28, 0xe4);
