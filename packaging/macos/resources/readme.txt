Before you install

- Warehouse is unsigned. This build carries no Apple Developer ID signature
  or notarisation ticket -- macOS Gatekeeper may warn you the first time each
  plug-in format loads. This is expected for an unsigned, freely distributed
  build; it is not a sign of tampering.

- Warehouse is free software, licensed under the GNU Affero General Public
  License v3 (AGPLv3, shown on the previous screen). The complete
  corresponding source code, including the exact JUCE revision used, is at:

    https://github.com/remicaesar/warehouse-reverb

- After installing, restart your DAW so it picks up the new or updated
  Audio Unit and VST3. Some hosts also need a manual rescan:

    Logic Pro     restart is enough; it rescans Audio Units at launch
    Ableton Live  Preferences > Plug-Ins > Rescan
    FL Studio     Options > Manage plugins > Find more plugins

- If you have previously copied Warehouse into your own
  ~/Library/Audio/Plug-Ins folders by hand, remove those copies. Otherwise
  your DAW will list Warehouse twice and may load the older one.

Uninstalling: quit your DAW, then remove
/Library/Audio/Plug-Ins/Components/Warehouse.component and/or
/Library/Audio/Plug-Ins/VST3/Warehouse.vst3.
