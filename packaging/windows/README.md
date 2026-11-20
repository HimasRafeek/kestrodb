# kestro-server as a Windows service

`kestro-server.exe` is a console program: it does not talk to the Service Control Manager
itself, so `sc.exe create` alone gives a service that fails to start (error 1053). Run it
under a service wrapper instead. Both below are free; both stop the service by sending Ctrl+C
to the server, which then stops cleanly (ends connections, writes its index snapshot, exits 0).

Make a data folder and a token first (an elevated PowerShell):

```powershell
$dir = 'C:\ProgramData\KestroDB'
New-Item -ItemType Directory -Force $dir | Out-Null
-join ((1..48) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) }) | Set-Content -NoNewline "$dir\token.txt"
icacls $dir /inheritance:r /grant:r 'SYSTEM:(OI)(CI)F' 'Administrators:(OI)(CI)F' | Out-Null
```

## NSSM (nssm.cc)

```powershell
nssm install KestroDB 'C:\Program Files\KestroDB\bin\kestro-server.exe'
nssm set KestroDB AppParameters '--db C:\ProgramData\KestroDB\kestro.kdb --create --tcp 127.0.0.1:7820 --token-file C:\ProgramData\KestroDB\token.txt'
nssm set KestroDB AppDirectory C:\ProgramData\KestroDB
nssm set KestroDB AppStdout C:\ProgramData\KestroDB\server.log
nssm set KestroDB AppStderr C:\ProgramData\KestroDB\server.log
nssm set KestroDB AppStopMethodSkip 6          # stop with Ctrl+C only (no WM_CLOSE / thread messages)
nssm set KestroDB AppStopMethodConsole 120000  # give the snapshot up to 2 minutes
nssm set KestroDB Start SERVICE_AUTO_START
nssm start KestroDB
```

## WinSW (github.com/winsw/winsw)

Rename `WinSW-x64.exe` to `kestrodb-service.exe`, put this `kestrodb-service.xml` beside it,
then run `kestrodb-service.exe install` and `kestrodb-service.exe start`:

```xml
<service>
  <id>KestroDB</id>
  <name>KestroDB</name>
  <description>KestroDB database server</description>
  <executable>C:\Program Files\KestroDB\bin\kestro-server.exe</executable>
  <arguments>--db C:\ProgramData\KestroDB\kestro.kdb --create --tcp 127.0.0.1:7820 --token-file C:\ProgramData\KestroDB\token.txt</arguments>
  <workingdirectory>C:\ProgramData\KestroDB</workingdirectory>
  <stoptimeout>120 sec</stoptimeout>
  <onfailure action="restart" delay="5 sec"/>
  <log mode="roll-by-size"/>
</service>
```

Both run the service as LocalSystem by default. To run it as a less privileged account, create
one (or use a virtual account such as `NT SERVICE\KestroDB` with NSSM's `ObjectName`), and give
it modify rights on `C:\ProgramData\KestroDB` only.
