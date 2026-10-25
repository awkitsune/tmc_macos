# The Legend of Zelda: The Minish Cap

> **This fork adds a native arm64 macOS port**, and an iPhone and iPad app. Setup, build steps and status are in [port/README.md](port/README.md); installing on an iPhone or iPad is [below](#iphone-and-ipad).


[![Build Status][jenkins-badge]][jenkins] [![Decompilation Progress][progress-badge]][progress] [![Contributors][contributors-badge]][contributors] [![Discord Channel][discord-badge]][discord]

[jenkins]: https://jenkins.deco.mp/job/TMC/job/master
[jenkins-badge]: https://img.shields.io/jenkins/build?jobUrl=https%3A%2F%2Fjenkins.deco.mp%2Fjob%2FTMC%2Fjob%2Fmaster

[progress]: https://zelda64.dev/games/tmc
[progress-badge]: https://img.shields.io/endpoint?url=https://zelda64.dev/assets/csv/progress-tmc-shield.json

[contributors]: https://github.com/zeldaret/tmc/graphs/contributors
[contributors-badge]: https://img.shields.io/github/contributors/zeldaret/tmc

[discord]: https://discord.zelda64.dev
[discord-badge]: https://img.shields.io/discord/688807550715560050?color=%237289DA&logo=discord&logoColor=%23FFFFFF

```diff
- WARNING! -

This repository is a work in progress, and while it can be used to make certain changes, it's still
constantly evolving. If you use it for modding purposes in its current state, please be aware that
the codebase can drastically change at any time. Also note that some parts of the ROM may not be
'shiftable' yet, so modifying them could be difficult at this point.
```

This is a WIP decompilation of The Legend of Zelda: The Minish Cap.
The purpose of the project is to recreate a source code base for the game from scratch, using information found inside the game along with static and/or dynamic analysis.

It can target the following ROMs:

* [**tmc.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=1841) `sha1: b4bd50e4131b027c334547b4524e2dbbd4227130`
* [**tmc_jp.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=1719) `sha1: 6c5404a1effb17f481f352181d0f1c61a2765c5d`
* [**tmc_eu.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=1734) `sha1: cff199b36ff173fb6faf152653d1bccf87c26fb7`
* [**tmc_demo_usa.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=x051) `sha1: 63fcad218f9047b6a9edbb68c98bd0dec322d7a1`
* [**tmc_demo_jp.gba**](https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=x430) `sha1: 9cdb56fa79bba13158b81925c1f3641251326412`

**Note:** This repository does not include any of the assets necessary to build the ROM.
A prior copy of the game is required to extract the needed assets.

Website: <https://zelda64.dev>

Discord: <https://discord.zelda64.dev>

Documentation: <https://zeldaret.github.io/tmc>

## iPhone and iPad

The port also runs on iPhone and iPad (iOS/iPadOS 16 or later), played with a game controller or a hardware
keyboard. It has no touch controls; a tap opens the settings. The app contains none of the game's data: on first
launch it asks for your own ROM of The Minish Cap (USA) and extracts what it needs.

A free Apple ID is enough to install it on your own device. It needs a Mac with Xcode (the iOS platform, from
Xcode > Settings > Components; the Simulator is not needed) and this repository set up as in
[port/README.md](port/README.md#setup) (assets extracted, the GBA build done, the Python environment).

1. **Add your Apple ID to Xcode.** Xcode > Settings > Accounts > **+** > Apple ID, and sign in. A free account shows
   up as "*Your Name* (Personal Team)".
2. **Connect the device.** Plug the iPhone or iPad into the Mac, unlock it and tap **Trust**.
3. **Turn on Developer Mode** on the device: Settings > Privacy & Security > Developer Mode > On, restart, then confirm
   with **Turn On**. (The switch appears once the device has been connected to a Mac with Xcode; if it is missing,
   open Xcode > Window > Devices and Simulators with the device plugged in.)
4. **Build and install.** From the repository root:

   ```bash
   port/scripts/ios_install.sh
   ```

   The script takes the team of the Apple ID in Xcode (or `--team TEAM_ID`) and the connected device (or
   `--device NAME`), configures the Xcode project, builds a signed Release app and installs it. By hand, the same is:

   ```bash
   cmake -S port -B build/ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DTMC_BAKE_DATA=OFF -DTMC_IOS_TEAM=TEAM_ID
   xcodebuild -project build/ios/tmc_native.xcodeproj -scheme tmc_ios -configuration Release -destination 'id=DEVICE_UDID' -allowProvisioningUpdates -allowProvisioningDeviceRegistration build
   xcrun devicectl device install app --device DEVICE_UDID build/ios/Release-iphoneos/TheMinishCap.app
   ```

   `TEAM_ID` is the 10-character ID of your team (`defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier`
   lists it as `teamID`); `DEVICE_UDID` comes from `xcrun devicectl list devices`.
5. **Trust the developer** the first time: the app will not open until you go to Settings > General > VPN & Device
   Management, tap your Apple ID under Developer App and tap **Trust**.
6. **Give it the ROM.** Put your `.gba` in the Files app (AirDrop, iCloud Drive, or Finder with the device
   connected), open The Minish Cap and tap **Choose ROM…**. The data is extracted once.
7. **Connect a controller** (Settings > Bluetooth on the device) or a keyboard. Press Home or hold ☰ on the controller,
   press Escape, or tap the screen for the settings.

With a free Apple ID:

- **The app stops opening after 7 days**, when its provisioning profile expires. Plug the device in and run
  `port/scripts/ios_install.sh` again; installing over the old app keeps the saves.
- **At most 3 apps** installed this way can be on a device at once (and Xcode makes at most 10 new app IDs a week). If
  Xcode reports the limit, delete another app you installed from Xcode. If it says the bundle ID is taken, pick your
  own: `TMC_IOS_BUNDLE_ID=io.github.yourname.tmc-ios port/scripts/ios_install.sh`.
- TestFlight and the App Store need the paid Apple Developer Program.

The game's save is `tmc.sav` in Files > On My iPad (or iPhone) > The Minish Cap, the same 8 KB file as the Mac app's
`~/Library/Application Support/tmc_macos/tmc.sav`: copy it either way to carry a game between Mac and iPad. Quick
save states are next to it in `states`.

## Installation

To set up the repository, see [INSTALL.md](INSTALL.md).

## Contributing

All contributions are welcome. This is a group effort, and even small contributions can make a difference.
Some tasks also don't require much knowledge to get started.

Most discussions happen on our [Discord Server](https://discord.zelda64.dev), where you are welcome to ask if you need help getting started, or if you have any questions regarding this project and other decompilation projects.
