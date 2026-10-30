// frame-vrsettings - read or write a live SteamVR setting through IVRSettings.
//
// SteamVR on the Frame reads ~/.config/openvr/config/steamvr.vrsettings once, at start, so a key written to the file
// only takes effect at the next SteamVR start. This goes through vrserver instead: the value applies now and SteamVR
// persists it to the file itself. The one that matters for streaming:
//
//   frame-vrsettings set steamvr defaultPerAppRefreshRate float 90    (the Frame runs OpenXR apps at 72 from the factory)
//
//   frame-vrsettings get <section> <key>                    (prints float|int|bool|string if any reads)
//   frame-vrsettings set <section> <key> float|int|bool|string <value>
//   frame-vrsettings remove <section> <key>
//
// Built by install.sh; run it on the headset while SteamVR is up.
#include <openvr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int usage() {
    fprintf(stderr, "usage: frame-vrsettings get <section> <key>\n"
                    "       frame-vrsettings set <section> <key> float|int|bool|string <value>\n"
                    "       frame-vrsettings remove <section> <key>\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 4) return usage();
    const std::string verb = argv[1];
    const char* section = argv[2];
    const char* key = argv[3];
    vr::EVRInitError ie;
    vr::VR_Init(&ie, vr::VRApplication_Utility);
    if (ie != vr::VRInitError_None) {
        fprintf(stderr, "frame-vrsettings: VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(ie));
        return 1;
    }
    vr::IVRSettings* s = vr::VRSettings();
    int rc = 0;
    vr::EVRSettingsError e = vr::VRSettingsError_None;
    if (verb == "get") {
        bool any = false;
        float f = s->GetFloat(section, key, &e);
        if (e == vr::VRSettingsError_None) { printf("float %g\n", f); any = true; }
        int32_t i = s->GetInt32(section, key, &e);
        if (e == vr::VRSettingsError_None) { printf("int %d\n", i); any = true; }
        bool b = s->GetBool(section, key, &e);
        if (e == vr::VRSettingsError_None) { printf("bool %s\n", b ? "true" : "false"); any = true; }
        char buf[1024] = {0};
        s->GetString(section, key, buf, sizeof buf, &e);
        if (e == vr::VRSettingsError_None) { printf("string %s\n", buf); any = true; }
        if (!any) { printf("unset (%s)\n", s->GetSettingsErrorNameFromEnum(e)); rc = 3; }
    } else if (verb == "set") {
        if (argc < 6) { vr::VR_Shutdown(); return usage(); }
        const std::string type = argv[4];
        const char* val = argv[5];
        if (type == "float") s->SetFloat(section, key, (float)atof(val), &e);
        else if (type == "int") s->SetInt32(section, key, atoi(val), &e);
        else if (type == "bool") s->SetBool(section, key, !strcmp(val, "true") || !strcmp(val, "1"), &e);
        else if (type == "string") s->SetString(section, key, val, &e);
        else { vr::VR_Shutdown(); return usage(); }
        if (e != vr::VRSettingsError_None) { fprintf(stderr, "set failed: %s\n", s->GetSettingsErrorNameFromEnum(e)); rc = 1; }
        else printf("set %s.%s = %s (%s)\n", section, key, val, type.c_str());
    } else if (verb == "remove") {
        s->RemoveKeyInSection(section, key, &e);
        if (e != vr::VRSettingsError_None) { fprintf(stderr, "remove failed: %s\n", s->GetSettingsErrorNameFromEnum(e)); rc = 1; }
        else printf("removed %s.%s\n", section, key);
    } else rc = usage();
    vr::VR_Shutdown();
    return rc;
}
