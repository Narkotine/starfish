#include <QtTest>

#include <windows.h>

#include "platform/win/autostart.hpp"

using namespace starfish::platform::win;

// Session 62: "Démarrer avec Windows" on scratch keys -- the user's Run key is
// never touched by this test.
namespace {
const QString kRoot = QStringLiteral("Software\\StarFish\\test-autostart");
const QString kRun = kRoot + QStringLiteral("\\Run");
const QString kApproved = kRoot + QStringLiteral("\\Approved");
void wipe()
{
    ::RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(kRoot.utf16()));
}
} // namespace

class TestAutostart : public QObject
{
    Q_OBJECT

private slots:
    void init() { wipe(); }
    void cleanup() { wipe(); }

    void offUntilAsked()
    {
        Autostart a(kRun, kApproved);
        QVERIFY(!a.read().registered);
        QVERIFY(!a.refresh(QStringLiteral("C:/x/starfish.exe"))); // never creates one
        QVERIFY(!a.read().registered);
    }

    void onWritesTheQuotedExeAndOffRemovesIt()
    {
        Autostart a(kRun, kApproved);
        QVERIFY(a.setEnabled(true, QStringLiteral("C:/Program Files/StarFish/starfish.exe")));
        const AutostartState s = a.read();
        QVERIFY(s.enabled());
        QCOMPARE(s.command, QStringLiteral("\"C:\\Program Files\\StarFish\\starfish.exe\""));
        QCOMPARE(Autostart::exeOf(s.command), QStringLiteral("C:\\Program Files\\StarFish\\starfish.exe"));
        QVERIFY(a.setEnabled(false, {}));
        QVERIFY(!a.read().registered);
    }

    void aMovedExeIsFollowed()
    {
        Autostart a(kRun, kApproved);
        a.setEnabled(true, QStringLiteral("C:/old/starfish.exe"));
        QVERIFY(a.refresh(QStringLiteral("D:/new/starfish.exe")));
        QCOMPARE(Autostart::exeOf(a.read().command), QStringLiteral("D:\\new\\starfish.exe"));
        QVERIFY(!a.refresh(QStringLiteral("d:/NEW/starfish.exe"))); // same file: untouched
    }

    void taskManagerDisabledIsShownAndLiftedWhenAskedHere()
    {
        Autostart a(kRun, kApproved);
        a.setEnabled(true, QStringLiteral("C:/x/starfish.exe"));
        const BYTE disabled[12] = {0x03};
        ::RegSetKeyValueW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(kApproved.utf16()), L"StarFish",
                          REG_BINARY, disabled, sizeof(disabled));
        QVERIFY(a.read().registered);
        QVERIFY(a.read().disabledByUser);
        QVERIFY(!a.read().enabled());
        QVERIFY(a.setEnabled(true, QStringLiteral("C:/x/starfish.exe")));
        QVERIFY(a.read().enabled());
    }
};

QTEST_MAIN(TestAutostart)
#include "test_autostart.moc"
