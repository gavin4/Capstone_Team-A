# Capstone_Team-A
Project for capstone team A. 

For meeting notes and project progress, please reference [the wiki, located here.](https://github.com/gavin4/Capstone_Team-A/wiki)



# Features

* This repository is meant to include ports for the currently existing filewtacher compatible with the Windows for Mozzila Firefox.
* The new ports will support Mac os following the "High Sierra" (Mac os 10.13) and beyond due to the inclusion of the APFS(Apple File System). Linux will aslo be supported.


## Linux Filewatcher
**All chnages for the linux filewatcher can be reflected in `Toolkit/Components/FileWatcher`. An implementation can be found in `NativeFileWatcherLNX.cpp` and `NativeFileWatcherLNX.h`.**

* The liniux filewatcher currently uses `inotify` as its event handler ot get systems level signals from the linux kernel. If you want more information on inotify please look at the [Inotify Man Page](http://man7.org/linux/man-pages/man7/inotify.7.html).  
* This implementation diverges from the windows version by excluding the implementation of completion ports and instead deciding to use file and watch descriptors to track watched files.

## MacOS Filewatcher
** All chnages for the linux filewatcher can be reflected in `Toolkit/Components/FileWatcher`. An implementation can be found in `NativeFileWatcherMac.cpp` and `NativeFileWatcherMac.h`.**

* The liniux filewatcher currently uses `inotify` as its event handler ot get systems level signals from the linux kernel. If you want more information on inotify please look at the [FSEvents API Docs](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/UsingtheFSEventsFramework/UsingtheFSEventsFramework.html#//apple_ref/doc/uid/TP40005289-CH4-SW4).  
* This implementation diverges from the windows version by excluding the implementation of completion ports and instead deciding to use file and watch descriptors to track watched files.

## Compiling

**Those who are building on mac, nbe warned, there are issues when building using OSX 10.14 SDK. Pleae follow this [Bugzilla report](https://bugzilla.mozilla.org/show_bug.cgi?id=1494022) to reverse this issue**.

* If you are not familiar with how to build firefox locally, please visit this website: [Building Firefox](https://developer.mozilla.org/en-US/docs/Mozilla/Developer_guide/Build_Instructions/Simple_Firefox_build).
* When you are ready to build firefox remember to use `./mach run` and then firfox will be ready for use.
