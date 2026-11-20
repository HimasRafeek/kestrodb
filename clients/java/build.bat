@echo off
rem Builds the KestroDB Java client with the JDK alone (Java 17+, no Maven or Gradle):
rem   build\classes   the compiled client (dev.kestrodb, dev.kestrodb.jdbc)
rem   kestrodb.jar    the library, with the JDBC driver registered for DriverManager
rem   build\test      the test program (test\dev\kestrodb\ClientTest.java), if present
setlocal
cd /d "%~dp0"
where javac >nul 2>nul || (echo javac not found: install a JDK 17 or newer & exit /b 1)
if exist build rmdir /s /q build
mkdir build\classes || exit /b 1
javac --release 17 -encoding UTF-8 -Xlint:all -Werror -d build\classes src\dev\kestrodb\*.java src\dev\kestrodb\jdbc\*.java || exit /b 1
xcopy /e /i /q /y resources build\classes >nul || exit /b 1
jar --create --file kestrodb.jar -C build\classes . || exit /b 1
if exist test\dev\kestrodb\ClientTest.java (
  mkdir build\test || exit /b 1
  javac --release 17 -encoding UTF-8 -Xlint:all -cp kestrodb.jar -d build\test test\dev\kestrodb\*.java || exit /b 1
)
echo built kestrodb.jar
