cask "p1600dn" do
  version "6.0.1"
  sha256 :no_check # set to the release archive's SHA-256 once v6.0.1 is published

  # There is no .pkg: the driver installs the way the manager installs it.
  # It builds the driver from the source archive, runs every test and installs
  # only when all of them pass.
  url "https://github.com/berkbb/p1600dn-macos-driver/archive/refs/tags/v#{version}.tar.gz"
  name "P1600dn Series Driver"
  desc "Native Apple Silicon CUPS driver for HP LaserJet P1600dn series printers"
  homepage "https://github.com/berkbb/p1600dn-macos-driver"

  depends_on arch: :arm64
  depends_on :macos

  installer script: {
    executable: "p1600dn-macos-driver-#{version}/p1600dn_manager.command",
    args:       ["--install"],
    sudo:       true,
  }

  uninstall script: {
    executable: "p1600dn-macos-driver-#{version}/p1600dn_manager.command",
    args:       ["--uninstall", "--yes"],
    sudo:       true,
  }

  caveats <<~EOS
    Installing builds the driver and runs all of its tests first; this takes a
    few minutes. Gutenprint is removed on the first installation, unless
    another printer still uses it.

    After connecting the printer by USB or network:
      1. Open System Settings -> Printers & Scanners and click "+".
      2. Select the HP LaserJet Professional P1600dn series printer.
         "HP LaserJet Professional P1600dn Series" is chosen as its driver.

    Test pages and printer settings: Printers & Scanners -> (printer) ->
    Options & Supplies -> Utility -> Open Printer Utility.
    AirPrint for iPhone and iPad is off until you turn it on:
      sudo "#{staged_path}/p1600dn-macos-driver-#{version}/p1600dn_manager.command" --enable-airprint
  EOS
end
