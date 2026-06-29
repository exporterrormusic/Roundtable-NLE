/*
 * NaturalSort.h — Premiere/Finder-style natural ordering helpers.
 *
 * Plain lexicographic QString ordering sorts "1, 10, 2" — a numeric-aware
 * collator sorts "1, 2, 10" the way users expect in a media bin. This header
 * provides a single cached QCollator and a NaturalTreeWidgetItem that uses it,
 * shared by the Project Bin's details tree and its icon grid so both views
 * order identically.
 *
 * Header-only / inline (#pragma once) so it is safe in the unity UI build.
 */

#pragma once

#include <QCollator>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace rt {

/// Shared, cached collator for natural ordering: numeric-aware ("2" < "13")
/// and case-insensitive. Constructing a QCollator is relatively expensive, so
/// it is cached. QCollator is not thread-safe to share across threads, but all
/// bin sorting runs on the GUI thread, so a single static instance is fine.
inline const QCollator& naturalCollator()
{
    static const QCollator collator = [] {
        QCollator c;
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    return collator;
}

/// True if a should sort before b under natural ordering.
inline bool naturalLess(const QString& a, const QString& b)
{
    return naturalCollator().compare(a, b) < 0;
}

/// QTreeWidgetItem that compares its sort-column text with natural ordering.
/// QTreeWidget invokes operator< on the LEFT operand, so EVERY row in a tree
/// that wants natural sorting must be this type — a plain QTreeWidgetItem on
/// the left would fall back to Qt's lexicographic compare and reintroduce the
/// "1, 10, 2" bug.
class NaturalTreeWidgetItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override
    {
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        return naturalLess(text(col), other.text(col));
    }
};

} // namespace rt
