"""The Multicam Timecode Sync Recorder installer: a per-user MSI built with WiX Toolset 3.14.

    py -3 crea-msi.py --build <build folder> --wix <WiX 3.14 folder> --uscita <output folder>

Takes from the build folder exactly what the program needs and writes mtsrec.wxs, then compiles it with
candle and light into «mtsrec-<version>.msi» (for example mtsrec-0.9-beta-6.msi).

Why an MSI and not Inno Setup (section 9.81 of the project document): SignPath signs the build
artifacts, while Inno Setup has to sign setup.tmp and its uninstaller while it compiles. SignPath opens
the MSI, signs mtsrec.exe inside it, then signs the MSI itself. Nothing is extracted to %TEMP% at install.

What the MSI does, as the Inno setup did up to 0.9 beta 5:
- installs for the current user only, in %LOCALAPPDATA%\\Programs\\mtsrec, without administrator rights
- Start menu folder «Multicam Timecode Sync Recorder» with the program, in browser, system check, user
  manual, beta tester guide and licenses; a desktop icon (DESKTOP_SHORTCUT=0 on the msiexec command
  line leaves it out)
- installing a new version replaces the old one (MajorUpgrade); settings in %USERPROFILE%\\mtsrec stay
- if WinISORecorder (the Inno setup up to 0.9 beta 5) is installed for the user, it uninstalls it
  silently first; installed for all users, it asks to uninstall it, because that needs an administrator
- offers to launch the program at the end
Blackmagic Desktop Video and the WebView2 Runtime are not checked here: the program checks them at every
start and offers the official download pages (section 9.77).

Every component has a stable GUID derived from its path, and a value under HKCU\\Software\\mtsrec as key
path, as Windows Installer wants for files installed in the user profile.
"""
import argparse
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

QUI = Path(__file__).resolve().parent
SORGENTE = QUI.parent
NOME = "Multicam Timecode Sync Recorder"
EDITORE = "Andrea Angeli"
# Never change it: it is what tells Windows that a new MSI replaces the old one.
UPGRADE_CODE = "{3B5E8C1A-7D24-4F6B-A0E9-52C71D8F4A36}"
SPAZIO = uuid.UUID("9d2f6c47-1b3e-4a85-8f70-6e4c2a1d5b93")   # for the component GUIDs
# The Inno setup of WinISORecorder, up to 0.9 beta 5 (its AppId)
INNO = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F2B8E4A-3C1D-4E7A-9B52-1A7D3C9E0F41}_is1"
REGISTRO = r"Software\mtsrec"


def versione():
    t = (SORGENTE / "VERSIONE.txt").read_text(encoding="utf-8").strip()
    m = re.fullmatch(r"(\d+)\.(\d+)(?: beta (\d+))?", t)
    if not m:
        sys.exit(f"VERSIONE.txt: «{t}» is not like «0.9 beta 6» or «1.0»")
    numeri = f"{m[1]}.{m[2]}.{m[3] or 0}"
    return t, numeri, t.replace(" ", "-")


def script_del_programma():
    """The same list as WINISO_SCRIPT in CMakeLists.txt: the build folder may also hold laboratory scripts."""
    c = (SORGENTE / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(WINISO_SCRIPT\s+(.*?)\)", c, re.S)
    return m[1].split()


def rinfresca(build):
    """Copies manual, guide, licenses, pages and scripts from the sources into the build folder.
    CMake copies them only after linking the program, so a manual regenerated later would have gone into
    the installer in its old version, as happened with the Inno setup on 17 November 2026 (section 9.77)."""
    script = SORGENTE / "script" if (SORGENTE / "script").is_dir() else SORGENTE.parent / "script"
    coppie = [(SORGENTE / "licenze" / "BETA TESTER GUIDE.txt", build / "BETA TESTER GUIDE.txt"),
              (SORGENTE / "manuale" / f"{NOME} User Manual.pdf", build / f"{NOME} User Manual.pdf")]
    coppie += [(SORGENTE / "licenze" / n, build / "licenses" / n) for n in ["LICENSE.txt", "THIRD-PARTY NOTICES.txt"]]
    coppie += [(p, build / "interfaccia" / p.relative_to(SORGENTE / "interfaccia"))
               for p in (SORGENTE / "interfaccia").rglob("*") if p.is_file()]
    coppie += [(script / s, build / "script" / s) for s in script_del_programma()]
    rinfrescati = 0
    for da, a in coppie:
        if not a.is_file() or a.read_bytes() != da.read_bytes():
            a.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(da, a)
            rinfrescati += 1
    if rinfrescati:
        print(f"{rinfrescati} files refreshed from the sources into the build folder")


def file_da_installare(build):
    voci = [build / "mtsrec.exe", build / "BETA TESTER GUIDE.txt", build / f"{NOME} User Manual.pdf"]
    voci += sorted(p for p in (build / "licenses").iterdir() if p.is_file() and p.name != "BETA LICENSE.txt")
    voci += sorted(p for p in (build / "interfaccia").rglob("*") if p.is_file())
    voci += [build / "script" / s for s in script_del_programma()]
    voci += sorted(p for p in (build / "python").rglob("*") if p.is_file() and "__pycache__" not in p.parts)
    mancano = [str(p) for p in voci if not p.is_file()]
    if mancano:
        sys.exit("missing from the build folder:\n  " + "\n  ".join(mancano))
    return voci


def ident(prefisso, testo):
    """A WiX identifier: letters, digits, dots and underscores, at most 72 characters."""
    return prefisso + uuid.uuid5(SPAZIO, testo.lower()).hex[:24]


def guid(testo):
    return "{" + str(uuid.uuid5(SPAZIO, testo.lower())).upper() + "}"


def rtf(testo):
    """The MIT license as RTF, for the license page of the installer."""
    corpo = []
    for par in testo.strip().split("\n\n"):
        r = " ".join(par.split())
        r = r.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}")
        corpo.append(r + "\\par\\par")
    return "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Segoe UI;}}\\fs18 " + "\n".join(corpo) + "}"


def wxs(build, voci, testo_versione, numeri):
    righe_dir, componenti, rif = [], [], []
    cartelle = {}   # relative path -> directory id

    def cartella(rel):
        if rel in cartelle:
            return cartelle[rel]
        padre = cartella(rel.parent) if rel.parent != Path(".") else "INSTALLFOLDER"
        i = ident("d", str(rel))
        cartelle[rel] = i
        righe_dir.append((padre, i, rel.name))
        return i

    for p in voci:
        rel = p.relative_to(build)
        d = cartella(rel.parent) if rel.parent != Path(".") else "INSTALLFOLDER"
        c = ident("c", str(rel))
        attr_exe = ' Id="mtsrec.exe"' if rel == Path("mtsrec.exe") else f' Id="{ident("f", str(rel))}"'
        componenti.append(
            f'    <DirectoryRef Id="{d}"><Component Id="{c}" Guid="{guid(str(rel))}">\n'
            f'      <File{attr_exe} Source={quoteattr(str(p))} />\n'
            f'      <RegistryValue Root="HKCU" Key="{REGISTRO}\\Files" Name={quoteattr(str(rel))} Type="integer" Value="1" KeyPath="yes" />\n'
            f'    </Component></DirectoryRef>')
        rif.append(c)

    # The folders: removed at uninstall, with the bytecode Python writes in script\__pycache__.
    pulizia = ['      <RemoveFolder Id="rmINSTALLFOLDER" Directory="INSTALLFOLDER" On="uninstall" />',
               '      <RemoveFolder Id="rmProgrammi" Directory="Programmi" On="uninstall" />',
               '      <RemoveFile Id="rmPyc" Directory="Pycache" Name="*.pyc" On="both" />',
               '      <RemoveFolder Id="rmPycache" Directory="Pycache" On="uninstall" />']
    for rel, i in cartelle.items():
        pulizia.append(f'      <RemoveFolder Id="rm{i}" Directory="{i}" On="uninstall" />')

    def albero(padre, rientro):
        out = []
        for p_, i, nome in righe_dir:
            if p_ == padre:
                figli = albero(i, rientro + "  ")
                extra = '\n' + rientro + '  <Directory Id="Pycache" Name="__pycache__" />' if nome == "script" and padre == "INSTALLFOLDER" else ""
                out.append(f'{rientro}<Directory Id="{i}" Name={quoteattr(nome)}>{extra}' + ("\n" + figli if figli else "") + f'\n{rientro}</Directory>')
        return "\n".join(out)

    exe = "[INSTALLFOLDER]mtsrec.exe"
    scorciatoie = [
        (NOME, "mtsrec.exe", ""),
        (f"{NOME} - in browser", "mtsrec.exe", "--browser"),
        (f"{NOME} - system check", "mtsrec.exe", "--controllo"),
    ]
    sc = []
    for k, (nome, _, arg) in enumerate(scorciatoie):
        argomenti = f' Arguments="{arg}"' if arg else ""
        sc.append(f'      <Shortcut Id="sc{k}" Name={quoteattr(nome)} Target="{exe}"{argomenti} WorkingDirectory="INSTALLFOLDER" Icon="mtsrec.ico" />')
    sc.append(f'      <Shortcut Id="scManuale" Name="{NOME} - user manual" Target="[INSTALLFOLDER]{NOME} User Manual.pdf" />')
    sc.append(f'      <Shortcut Id="scGuida" Name="{NOME} - beta tester guide" Target="[INSTALLFOLDER]BETA TESTER GUIDE.txt" />')
    sc.append(f'      <Shortcut Id="scLicenze" Name="{NOME} - licenses" Target="[{cartelle[Path("licenses")]}]" />')

    return f'''<?xml version="1.0" encoding="utf-8"?>
<!-- Generated by installatore\\crea-msi.py from the build folder: do not edit by hand. -->
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi" xmlns:util="http://schemas.microsoft.com/wix/UtilExtension">
  <Product Id="*" Name="{NOME}" Language="1033" Version="{numeri}" Manufacturer="{EDITORE}" UpgradeCode="{UPGRADE_CODE}">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perUser" InstallPrivileges="limited"
             Description="{NOME} {escape(testo_versione)} installer" Comments="Free software under the MIT License" />
    <MajorUpgrade AllowSameVersionUpgrades="yes" Schedule="afterInstallInitialize"
                  DowngradeErrorMessage="A newer version of {NOME} is already installed." />
    <MediaTemplate EmbedCab="yes" CompressionLevel="high" />

    <!-- WinISORecorder, the Inno setup up to 0.9 beta 5, installed for the user (the default). Its own
         uninstaller cannot be used from here: it hands over to a copy of itself in %TEMP% and returns at
         once, and the Start menu folder stayed behind in every test (18 November 2026). So the MSI removes
         the program folder and the entry in Installed apps. The old shortcuts in the Start menu and on the
         desktop are removed by mtsrec.exe at its first start (togli_collegamenti_vecchi in macchina.cpp):
         a custom action here could not see the Start menu folder, though it was there. Settings in
         %USERPROFILE% are not touched.
         The folder is removed only if its name is WinISORecorder, as the setup proposed; if the tester chose
         another folder, its uninstaller runs instead, so nothing else is ever deleted.
         Installed for all users: removing it needs administrator rights, so the MSI asks the user to do it. -->
    <Property Id="INNO_CARTELLA"><RegistrySearch Id="cercaInnoUtente" Root="HKCU" Key="{INNO}" Name="InstallLocation" Type="raw" /></Property>
    <Property Id="INNO_MACCHINA"><RegistrySearch Id="cercaInnoMacchina" Root="HKLM" Key="{INNO}" Name="UninstallString" Type="raw" Win64="yes" /></Property>
    <CustomAction Id="TogliWinISORecorder" Directory="LocalAppDataFolder" Execute="deferred" Impersonate="yes" Return="ignore"
                  ExeCommand='"[System64Folder]cmd.exe" /c rd /s /q "[INNO_CARTELLA]." &amp; reg delete "HKCU\\{INNO}" /f' />
    <SetProperty Id="INNO_DISINSTALLA" Value="[INNO_CARTELLA]unins000.exe" After="AppSearch" Sequence="both">INNO_CARTELLA</SetProperty>
    <CustomAction Id="DisinstallaWinISORecorder" Property="INNO_DISINSTALLA" ExeCommand="/VERYSILENT /SUPPRESSMSGBOXES /NORESTART"
                  Execute="deferred" Impersonate="yes" Return="ignore" />
    <InstallExecuteSequence>
      <Custom Action="TogliWinISORecorder" Before="RemoveFiles">INNO_CARTELLA ~&gt;&gt; "\\WinISORecorder\\" AND NOT REMOVE</Custom>
      <Custom Action="DisinstallaWinISORecorder" Before="RemoveFiles">INNO_CARTELLA AND NOT (INNO_CARTELLA ~&gt;&gt; "\\WinISORecorder\\") AND NOT REMOVE</Custom>
    </InstallExecuteSequence>
    <Condition Message="WinISORecorder, the previous name of this program, is installed for all users. Uninstall it first from Windows Settings &gt; Apps &gt; WinISORecorder (it needs administrator rights), then run this installer again. Your settings are kept.">
      Installed OR NOT INNO_MACCHINA
    </Condition>
    <Condition Message="{NOME} needs Windows 10 or later, 64-bit.">Installed OR (VersionNT64 &gt;= 603)</Condition>

    <Icon Id="mtsrec.ico" SourceFile={quoteattr(str(SORGENTE / "src" / "mtsrec.ico"))} />
    <Property Id="ARPPRODUCTICON" Value="mtsrec.ico" />
    <Property Id="ARPURLINFOABOUT" Value="https://github.com/angeliandrea/mtsrec" />
    <Property Id="DESKTOP_SHORTCUT" Value="1" />

    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="LocalAppDataFolder">
        <Directory Id="Programmi" Name="Programs">
          <Directory Id="INSTALLFOLDER" Name="mtsrec">
{albero("INSTALLFOLDER", "            ")}
          </Directory>
        </Directory>
      </Directory>
      <Directory Id="ProgramMenuFolder">
        <Directory Id="Menu" Name="{NOME}" />
      </Directory>
      <Directory Id="DesktopFolder" />
    </Directory>

{chr(10).join(componenti)}

    <DirectoryRef Id="INSTALLFOLDER"><Component Id="cPulizia" Guid="{guid("pulizia")}">
{chr(10).join(pulizia)}
      <RegistryValue Root="HKCU" Key="{REGISTRO}" Name="InstallFolder" Type="string" Value="[INSTALLFOLDER]" KeyPath="yes" />
    </Component></DirectoryRef>
    <DirectoryRef Id="Menu"><Component Id="cMenu" Guid="{guid("menu")}">
{chr(10).join(sc)}
      <RemoveFolder Id="rmMenu" On="uninstall" />
      <!-- the desktop icon of WinISORecorder, if its uninstaller leaves it -->
      <RemoveFile Id="rmVecchioDesktop" Directory="DesktopFolder" Name="WinISORecorder.lnk" On="install" />
      <RegistryValue Root="HKCU" Key="{REGISTRO}" Name="StartMenu" Type="integer" Value="1" KeyPath="yes" />
    </Component></DirectoryRef>
    <DirectoryRef Id="DesktopFolder"><Component Id="cDesktop" Guid="{guid("desktop")}">
      <Condition>DESKTOP_SHORTCUT = 1</Condition>
      <Shortcut Id="scDesktop" Name="{NOME}" Target="{exe}" WorkingDirectory="INSTALLFOLDER" Icon="mtsrec.ico" />
      <RegistryValue Root="HKCU" Key="{REGISTRO}" Name="DesktopShortcut" Type="integer" Value="1" KeyPath="yes" />
    </Component></DirectoryRef>

    <Feature Id="Programma" Title="{NOME}" Level="1">
{chr(10).join(f'      <ComponentRef Id="{c}" />' for c in rif)}
      <ComponentRef Id="cPulizia" />
      <ComponentRef Id="cMenu" />
      <ComponentRef Id="cDesktop" />
    </Feature>

    <WixVariable Id="WixUILicenseRtf" Value="licenza.rtf" />
    <Property Id="WIXUI_EXITDIALOGOPTIONALCHECKBOXTEXT" Value="Launch {NOME}" />
    <Property Id="WIXUI_EXITDIALOGOPTIONALCHECKBOX" Value="1" />
    <Property Id="WixShellExecTarget" Value="[#mtsrec.exe]" />
    <CustomAction Id="Avvia" BinaryKey="WixCA" DllEntry="WixShellExec" Impersonate="yes" />
    <UI>
      <UIRef Id="WixUI_Minimal" />
      <Publish Dialog="ExitDialog" Control="Finish" Event="DoAction" Value="Avvia">WIXUI_EXITDIALOGOPTIONALCHECKBOX = 1 and NOT Installed</Publish>
    </UI>
  </Product>
</Wix>
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", required=True, type=Path)
    ap.add_argument("--wix", required=True, type=Path)
    ap.add_argument("--uscita", required=True, type=Path, help="keep it outside the source tree")
    a = ap.parse_args()

    testo, numeri, nome_file = versione()
    rinfresca(a.build)
    voci = file_da_installare(a.build)
    a.uscita.mkdir(parents=True, exist_ok=True)
    lavoro = a.uscita / "wix"
    lavoro.mkdir(exist_ok=True)
    (lavoro / "mtsrec.wxs").write_text(wxs(a.build, voci, testo, numeri), encoding="utf-8")
    (lavoro / "licenza.rtf").write_text(rtf((SORGENTE / "licenze" / "LICENSE.txt").read_text(encoding="utf-8")), encoding="ascii")
    msi = a.uscita / f"mtsrec-{nome_file}.msi"
    ext = ["-ext", "WixUIExtension", "-ext", "WixUtilExtension"]
    subprocess.run([str(a.wix / "candle.exe"), "-nologo", "-arch", "x64", *ext, "-out", str(lavoro / "mtsrec.wixobj"),
                    str(lavoro / "mtsrec.wxs")], check=True, cwd=lavoro)
    subprocess.run([str(a.wix / "light.exe"), "-nologo", *ext, "-cultures:en-us", "-spdb", "-sice:ICE91", "-sice:ICE61",
                    "-out", str(msi), str(lavoro / "mtsrec.wixobj")], check=True, cwd=lavoro)
    print(f"{msi.name}: {len(voci)} files, version {numeri} ({testo}), {msi.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
