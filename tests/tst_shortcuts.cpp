#include <QtTest>
#include <QTemporaryDir>
#include <QWidget>
#include "shortcutsettings.h"
#include "windowbridge.h"

class ShortcutTest : public QObject {
    Q_OBJECT
private slots:
    void validation() {
        const auto defaults = ShortcutSettings::defaults(); QString normalized;
        QVERIFY(ShortcutSettings::validate(defaults, "play", "Ctrl+P", &normalized).isEmpty());
        QCOMPARE(normalized, QString("Ctrl+P"));
        QVERIFY(!ShortcutSettings::validate(defaults, "play", "M", &normalized).isEmpty());
        for (const auto &key : {"Esc", "Alt+F4", "Ctrl+F", "Meta+P", "Ctrl+K, Ctrl+C", "Shift"})
            QVERIFY2(!ShortcutSettings::validate(defaults, "play", key, &normalized).isEmpty(), key);
        QVERIFY(!ShortcutSettings::validate(defaults, "unknown", "P", &normalized).isEmpty());
        QVERIFY(ShortcutSettings::validate(defaults, "play", "", &normalized).isEmpty());
        QVERIFY(normalized.isEmpty());
    }
    void persistenceAndBroadcast() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("LAMBDA_DATA_DIR", directory.path().toUtf8());
        QWidget window; WindowBridge home(&window), player(&window);
        QSignalSpy homeChanged(&home, &WindowBridge::shortcutsChanged);
        QSignalSpy playerChanged(&player, &WindowBridge::shortcutsChanged);
        QVERIFY(home.setShortcut("play", "Ctrl+P").value("ok").toBool());
        QCOMPARE(homeChanged.count(), 1); QCOMPARE(playerChanged.count(), 1);
        QCOMPARE(player.shortcuts().value("play").toString(), QString("Ctrl+P"));
        QVERIFY(!player.setShortcut("mute", "Ctrl+P").value("ok").toBool());
        QCOMPARE(homeChanged.count(), 1);
        WindowBridge restarted; QCOMPARE(restarted.shortcuts().value("play").toString(), QString("Ctrl+P"));
        QVERIFY(home.setShortcut("play", "").value("ok").toBool());
        WindowBridge disabled; QVERIFY(disabled.shortcuts().value("play").toString().isEmpty());
        home.resetShortcuts(); QCOMPARE(player.shortcuts(), ShortcutSettings::defaults());
        WindowBridge reset; QCOMPARE(reset.shortcuts(), ShortcutSettings::defaults());
        home.setKeyboardInputActive(true); QVERIFY(home.keyboardInputActive());
        home.setKeyboardInputActive(false); QVERIFY(!home.keyboardInputActive());
        qunsetenv("LAMBDA_DATA_DIR");
    }
};
QTEST_MAIN(ShortcutTest)
#include "tst_shortcuts.moc"
