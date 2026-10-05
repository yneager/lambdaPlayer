#pragma once
#include <QJsonObject>
#include <QKeySequence>
#include <QSettings>
#include <QString>

namespace ShortcutSettings {
inline QJsonObject defaults()
{
    return {{"open", "Ctrl+O"}, {"play", "Space"}, {"fullscreen", "F"}, {"mute", "M"},
            {"seekBack", "Left"}, {"seekForward", "Right"}, {"volumeUp", "Up"}, {"volumeDown", "Down"},
            {"next", "Ctrl+Right"}, {"home", "Ctrl+H"}, {"interpolation", "Ctrl+I"},
            {"subtitleEarlier", "G"}, {"subtitleLater", "H"}, {"loadSubtitle", "Ctrl+Shift+S"}};
}
inline QString validate(const QJsonObject &bindings, const QString &action, const QString &text, QString *normalized)
{
    if (!defaults().contains(action)) return "Unknown shortcut action.";
    const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    if (!text.trimmed().isEmpty() && (sequence.isEmpty() || sequence.count() != 1
        || sequence[0].key() == Qt::Key_unknown || sequence[0].key() == 0
        || sequence[0].key() == Qt::Key_Shift || sequence[0].key() == Qt::Key_Control
        || sequence[0].key() == Qt::Key_Alt || sequence[0].key() == Qt::Key_Meta))
        return "Choose a single key or key combination.";
    if (!sequence.isEmpty()) {
        const auto key = sequence[0];
        if (key.keyboardModifiers().testFlag(Qt::MetaModifier)
            || sequence == QKeySequence("Esc") || sequence == QKeySequence("Alt+F4")
            || sequence == QKeySequence("Ctrl+F") || sequence == QKeySequence("/"))
            return "This shortcut is reserved for navigation or Windows.";
        for (auto it = bindings.begin(); it != bindings.end(); ++it)
            if (it.key() != action && !it.value().toString().isEmpty()
                && QKeySequence(it.value().toString()) == sequence)
                return "This shortcut is already assigned to another action.";
    }
    *normalized = sequence.toString(QKeySequence::PortableText);
    return {};
}
inline QJsonObject load(QSettings &settings)
{
    auto bindings = defaults();
    for (auto it = bindings.begin(); it != bindings.end(); ++it)
        it.value() = settings.value("shortcuts/" + it.key(), it.value().toString()).toString();
    return bindings;
}
}
