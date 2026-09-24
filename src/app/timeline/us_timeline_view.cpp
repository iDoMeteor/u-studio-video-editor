#include "us_timeline_view.h"

struct _UsTimelineView
{
    GtkWidget parent_instance;
    ustudio::app::timeline::SnapshotFunc *draw;
    ustudio::app::timeline::ResizeFunc *resized;
};

namespace {

// Registered by hand rather than with G_DEFINE_FINAL_TYPE, whose expansion
// is full of C casts the project's -Wold-style-cast rejects.
GtkWidgetClass *gParentClass = nullptr;

UsTimelineView *self(GtkWidget *widget)
{
    return reinterpret_cast<UsTimelineView *>(widget);
}

void usTimelineViewSnapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    UsTimelineView *view = self(widget);
    if (view->draw && *view->draw)
        (*view->draw)(snapshot, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
}

void usTimelineViewSizeAllocate(GtkWidget *widget, int width, int height, int)
{
    // GtkPopover children (the context menu, the rename box) need this on
    // every allocation of their parent (gtk_popover_present's docs).
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_POPOVER(child))
            gtk_popover_present(GTK_POPOVER(child));
    }
    UsTimelineView *view = self(widget);
    if (view->resized && *view->resized)
        (*view->resized)(width, height);
}

void usTimelineViewDispose(GObject *object)
{
    GtkWidget *widget = GTK_WIDGET(object);
    while (GtkWidget *child = gtk_widget_get_first_child(widget))
        gtk_widget_unparent(child);
    G_OBJECT_CLASS(gParentClass)->dispose(object);
}

void usTimelineViewFinalize(GObject *object)
{
    UsTimelineView *view = self(GTK_WIDGET(object));
    delete view->draw;
    delete view->resized;
    G_OBJECT_CLASS(gParentClass)->finalize(object);
}

void usTimelineViewClassInit(gpointer klass, gpointer)
{
    gParentClass = static_cast<GtkWidgetClass *>(g_type_class_peek_parent(klass));
    GObjectClass *objectClass = G_OBJECT_CLASS(klass);
    objectClass->dispose = usTimelineViewDispose;
    objectClass->finalize = usTimelineViewFinalize;
    GtkWidgetClass *widgetClass = GTK_WIDGET_CLASS(klass);
    widgetClass->snapshot = usTimelineViewSnapshot;
    widgetClass->size_allocate = usTimelineViewSizeAllocate;
    gtk_widget_class_set_css_name(widgetClass, "ustimelineview");
}

void usTimelineViewInit(GTypeInstance *instance, gpointer)
{
    UsTimelineView *view = reinterpret_cast<UsTimelineView *>(instance);
    view->draw = nullptr;
    view->resized = nullptr;
}

} // namespace

GType us_timeline_view_get_type()
{
    static const GType type = g_type_register_static_simple(
        GTK_TYPE_WIDGET, g_intern_static_string("UsTimelineView"), sizeof(UsTimelineViewClass), usTimelineViewClassInit,
        sizeof(UsTimelineView), usTimelineViewInit, G_TYPE_FLAG_FINAL);
    return type;
}

namespace ustudio::app::timeline {

GtkWidget *newTimelineView(SnapshotFunc draw, ResizeFunc resized)
{
    auto *view = static_cast<UsTimelineView *>(g_object_new(US_TYPE_TIMELINE_VIEW, nullptr));
    view->draw = new SnapshotFunc(std::move(draw));
    view->resized = new ResizeFunc(std::move(resized));
    return GTK_WIDGET(view);
}

} // namespace ustudio::app::timeline
