#include "videopresentation.h"
#include <QtTest>

class VideoPresentationTest : public QObject
{
    Q_OBJECT
private slots:
    void repeatedPaintsAndOvertakenFrames() {
        VideoPresentationTracker counter;
        QVERIFY(!counter.swapped(-1));
        QVERIFY(counter.swapped(1));
        for (int redraw = 0; redraw < 144; ++redraw) QVERIFY(!counter.swapped(1));
        // Frames 2 and 3 were painted or queued but overtaken before a swap.
        QVERIFY(counter.swapped(4));
        QVERIFY(!counter.swapped(4));
    }
    void heldCutFramesAndGeneratedFrames() {
        VideoPresentationTracker counter;
        QVERIFY(counter.swapped(10));
        // The cut detector holds source A at each intermediate output tick.
        QVERIFY(!counter.swapped(10));
        QVERIFY(!counter.swapped(10));
        QVERIFY(counter.swapped(11));
        QVERIFY(counter.swapped(11.4));
        QVERIFY(counter.swapped(11.8));
        QVERIFY(counter.swapped(12));
    }
    void resetAfterSeek() {
        VideoPresentationTracker counter;
        QVERIFY(counter.swapped(100));
        counter.reset();
        QVERIFY(counter.swapped(100));
        QVERIFY(counter.swapped(0));
    }
    void normalCadenceSurvivesPressure() {
        QCOMPARE(reusableOutputRate(24,48,240,.5),48.0);
        QCOMPARE(reusableOutputRate(24,60,240,.5),60.0);
        QCOMPARE(reusableOutputRate(30,60,144,.5),60.0);
        QCOMPARE(reusableOutputRate(24,240,240,.5),120.0);
        QCOMPARE(reusableOutputRate(24,120,144,.5),60.0);
        QCOMPARE(reusableOutputRate(24,240,60,.5),60.0);
        QCOMPARE(reusableOutputRate(24,48,30,.5),30.0);
        QCOMPARE(reusableOutputRate(60,240,240,.5),120.0);
    }
};
QTEST_APPLESS_MAIN(VideoPresentationTest)
#include "tst_videopresentation.moc"
