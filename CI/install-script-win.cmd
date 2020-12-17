rem ---------------------------------------------------------------------
rem if exist Qt_5.10.1.7z (curl -kLO https://cdn-fastly.obsproject.com/downloads/Qt_5.10.1.7z -f --retry 5 -z Qt_5.10.1.7z) else (curl -kLO https://cdn-fastly.obsproject.com/downloads/Qt_5.10.1.7z -f --retry 5 -C -)
rem 7z x Qt_5.10.1.7z -oQt
rem set QTDIR64=%CD%\Qt\5.10.1\msvc2017_64
rem set QTDIR64=C:\Qt\Qt5.13.2\5.13.2\msvc2017_64
rem ---------------------------------------------------------------------
if exist dependencies2017.zip (curl -kLO https://cdn-fastly.obsproject.com/downloads/dependencies2017.zip -f --retry 5 -z dependencies2017.zip) else (curl -kLO https://cdn-fastly.obsproject.com/downloads/dependencies2017.zip -f --retry 5 -C -)
set DEPS_OUTPUT_DIR=dependencies2017
if not exist %DEPS_OUTPUT_DIR% (7z x dependencies2017.zip -o%DEPS_OUTPUT_DIR%)
set DepsPath64=%CD%\dependencies2017\win64
rem ---------------------------------------------------------------------
if exist vlc.zip (curl -kLO https://cdn-fastly.obsproject.com/downloads/vlc.zip -f --retry 5 -z vlc.zip) else (curl -kLO https://cdn-fastly.obsproject.com/downloads/vlc.zip -f --retry 5 -C -)
set VLC_OUTPUT_DIR=vlc
if not exist %VLC_OUTPUT_DIR% (7z x vlc.zip -o%VLC_OUTPUT_DIR%)
set VLCPath=%CD%\vlc
rem ---------------------------------------------------------------------
rem if exist cef_binary_%CEF_VERSION%_windows64.zip (curl -kLO https://cdn-fastly.obsproject.com/downloads/cef_binary_%CEF_VERSION%_windows64.zip -f --retry 5 -z cef_binary_%CEF_VERSION%_windows64.zip) else (curl -kLO https://cdn-fastly.obsproject.com/downloads/cef_binary_%CEF_VERSION%_windows64.zip -f --retry 5 -C -)
rem 7z x cef_binary_%CEF_VERSION%_windows64.zip -oCEF_64
rem set CEF_64=%CD%\CEF_64\cef_binary_%CEF_VERSION%_windows64
rem ---------------------------------------------------------------------
rem if exist libWebRTC-79-win.tar.gz (curl -kLO https://libwebrtc-community-builds.s3.amazonaws.com/libWebRTC-79-win.tar.gz -f --retry 5 -z libWebRTC-79-win.tar.gz) else (curl -kLO https://libwebrtc-community-builds.s3.amazonaws.com/libWebRTC-79-win.tar.gz -f --retry 5 -C -)
rem tar -xzf libWebRTC-79-win.tar.gz
rem set libwebrtcPath=%CD%\libWebRTC-79\cmake
rem set libwebrtcPath=C:\tools\webrtc\m79\webrtc\cmake
rem ---------------------------------------------------------------------
if exist openssl-1.1.tgz (curl -kLO https://libwebrtc-community-builds.s3.amazonaws.com/openssl-1.1.tgz -f --retry 5 -z openssl-1.1.tgz) else                 (curl -kLO https://libwebrtc-community-builds.s3.amazonaws.com/openssl-1.1.tgz -f --retry 5 -C -)
if not exist openssl-1.1 (tar -xzf openssl-1.1.tgz)
set opensslPath=%CD%\openssl-1.1\x64
rem ---------------------------------------------------------------------
set build_config=RelWithDebInfo
