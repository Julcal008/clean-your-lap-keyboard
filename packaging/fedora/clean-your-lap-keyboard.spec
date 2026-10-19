Name:           clean-your-lap-keyboard
Version:        0.1.1
Release:        1%{?dist}
Summary:        Temporarily disable the built-in laptop keyboard for cleaning

License:        GPL-3.0-or-later
URL:            https://github.com/julcal008/clean-your-lap-keyboard
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson >= 0.62
BuildRequires:  ninja-build
BuildRequires:  gettext
BuildRequires:  desktop-file-utils
BuildRequires:  appstream
BuildRequires:  pkgconfig(gtk4) >= 4.8
BuildRequires:  pkgconfig(libadwaita-1) >= 1.2
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(libudev)

Requires:       gtk4
Requires:       libadwaita
Requires:       polkit
Requires:       systemd-libs

%description
Clean Your Lap Keyboard temporarily grabs the internal keyboard of a laptop
using evdev, so the keys can be wiped without typing anything. The touchpad and
external keyboards keep working.

The keyboard is released automatically after the selected duration, when the
Stop button is pressed, when Esc is held for two seconds, and when the screen
locks, the computer suspends, or the session becomes inactive. A small
privileged helper performs only the device grab and never reads key events.

%prep
%autosetup

%build
%meson -Dtests=true
%meson_build

%install
%meson_install

%find_lang %{name}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.julcal008.CleanYourLapKeyboard.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/io.github.julcal008.CleanYourLapKeyboard.metainfo.xml
%meson_test

%files -f %{name}.lang
%license LICENSE
%doc README.md README.es.md CHANGELOG.md docs/ARCHITECTURE.md docs/ARQUITECTURA.es.md
%{_bindir}/cylk
%{_mandir}/man1/cylk.1*
%dir %{_libexecdir}/clean-your-lap-keyboard
%{_libexecdir}/clean-your-lap-keyboard/cylk-helper
%{_datadir}/applications/io.github.julcal008.CleanYourLapKeyboard.desktop
%{_datadir}/metainfo/io.github.julcal008.CleanYourLapKeyboard.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.julcal008.CleanYourLapKeyboard.svg
%{_datadir}/polkit-1/actions/io.github.julcal008.CleanYourLapKeyboard.policy

%changelog
* Mon Oct 19 2026 Julcal008 <julcalderon3008@gmail.com> - 0.1.1-1
- Also block media keys (volume, microphone, brightness) and the power button
  while cleaning.

* Mon Oct 19 2026 Julcal008 <julcalderon3008@gmail.com> - 0.1.0-1
- First release.
