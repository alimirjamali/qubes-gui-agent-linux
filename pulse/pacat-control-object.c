#include <string.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include "pacat-control-object.h"

#include "pacat-control-stub.h"

/* Properties */
enum
{
	PROP_0,
	PROP_REC_ALLOWED
};

enum
{
	SIGNAL_REC_ALLOWED_CHANGED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE(PacatControl, pacat_control, G_TYPE_OBJECT)

#define PACAT_CONTROL_TYPE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), PACAT_CONTROL_TYPE, userdata))

static void pacat_control_finalize (GObject *object)
{
	//PacatControl *p = PACAT_CONTROL (object);

	(G_OBJECT_CLASS (pacat_control_parent_class)->finalize) (object);
}

static void pacat_control_init (PacatControl *obj)
{
}

static void pacat_control_set_property (GObject      *object,
		guint         prop_id,
		const GValue *value,
		GParamSpec   *pspec)
{
	PacatControl *p;

	p = PACAT_CONTROL (object);

	switch (prop_id)
	{
		case PROP_REC_ALLOWED:
			assert(p->u);
			p->u->rec_allowed = g_value_get_boolean(value);
			pacat_log("Setting audio-input to %s", p->u->rec_allowed ? "enabled" : "disabled");
			if (p->u->rec_allowed && p->u->rec_requested) {
				pacat_log("Recording start");
				pa_stream_cork(p->u->rec_stream, 0, NULL, NULL);
			} else if (!p->u->rec_allowed &&
					(p->u->rec_requested || !pa_stream_is_corked(p->u->rec_stream))) {
				pacat_log("Recording stop");
				pa_stream_cork(p->u->rec_stream, 1, NULL, NULL);
			}
			/* notify about the change */
			g_signal_emit(object, signals[SIGNAL_REC_ALLOWED_CHANGED], 0, p->u->rec_allowed);
			break;

		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void pacat_control_get_property (GObject      *object,
		guint         prop_id,
		GValue       *value,
		GParamSpec   *pspec)
{
	PacatControl *p;

	p = PACAT_CONTROL (object);

	switch (prop_id)
	{
		case PROP_REC_ALLOWED:
			assert(p->u);
			g_value_set_boolean (value, p->u->rec_allowed);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void pacat_control_class_init (PacatControlClass *p_class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (p_class);

	dbus_g_object_type_install_info (PACAT_CONTROL_TYPE,
			&dbus_glib_pacat_control_object_info);

	/* pointer to userdata struct */
	g_type_class_add_private(p_class, sizeof(void*));

	gobject_class->finalize = pacat_control_finalize;
	gobject_class->set_property = pacat_control_set_property;
	gobject_class->get_property = pacat_control_get_property;

	g_object_class_install_property(gobject_class, PROP_REC_ALLOWED,
			g_param_spec_boolean("rec_allowed", "Allow use audio source", "Allow use audio source", FALSE, G_PARAM_READWRITE));

	signals[SIGNAL_REC_ALLOWED_CHANGED] =
		g_signal_new ("rec_allowed_changed",
				G_OBJECT_CLASS_TYPE (p_class),
				G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
				0,
				NULL, NULL,
				g_cclosure_marshal_VOID__BOOLEAN,
				G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

}

int dbus_init(struct userdata *u) {
	DBusGProxy *busProxy = NULL;
	GError *error = NULL;
	char obj_path[1024];
	PacatControl *pc;
	int result, ret;

	g_type_init ();
	g_thread_init (NULL);
	dbus_g_thread_init ();
	error = NULL;

	if (!(u->dbus = dbus_g_bus_get (DBUS_BUS_SESSION, &error))) {
		goto fail;
	}

	busProxy = dbus_g_proxy_new_for_name(u->dbus,
			DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS);
	if (busProxy == NULL) {
		pacat_log("Failed to get a proxy for D-Bus");
		goto fail;
	}

	/* Attempt to register the well-known name.
	   The RPC call requires two parameters:
	   - arg0: (D-Bus STRING): name to request
	   - arg1: (D-Bus UINT32): flags for registration.
	   (please see "org.freedesktop.DBus.RequestName" in
		http://dbus.freedesktop.org/doc/dbus-specification.html)
		Will return one uint32 giving the result of the RPC call.
		We're interested in 1 (we're now the primary owner of the name)
		or 4 (we were already the owner of the name)

		The function will return FALSE if it sets the GError. */
	if (snprintf(obj_path, sizeof(obj_path), "org.QubesOS.Audio.%s", u->name) >= sizeof(obj_path)) {
		pacat_log("VM name too long");
		goto fail;
	}
	if (!dbus_g_proxy_call(busProxy,
				"RequestName",
				&error,
				G_TYPE_STRING, obj_path, /* name */
				G_TYPE_UINT, 0, /* flags */
				G_TYPE_INVALID, /* end of input args */
				G_TYPE_UINT, &result, /* result */
				G_TYPE_INVALID)) {
		pacat_log("D-Bus.RequestName RPC failed", error->message);
		g_error_free (error);
		goto fail;
	}
	/* Check the result code of the registration RPC. */
	if (result != 1 && result != 4) {
		pacat_log("Failed to get the primary well-known name.");
		goto fail;
	}
	if (!(u->pacat_control = g_object_new (PACAT_CONTROL_TYPE, NULL))) {
		pacat_log("failed to create pacat_control object");
		goto fail;
	}

	pc = PACAT_CONTROL(u->pacat_control);
	pc->u = u;

	if (snprintf(obj_path, sizeof(obj_path), "/org/qubesos/audio/%s", u->name) >= sizeof(obj_path)) {
		pacat_log("VM name too long");
		goto fail;
	}
	dbus_g_connection_register_g_object (u->dbus,
			obj_path, u->pacat_control);

	ret = 0;
	goto finish;

fail:

	if (u->pacat_control) {
		dbus_g_connection_unregister_g_object(u->dbus, u->pacat_control);
		g_object_unref(u->pacat_control);
		u->pacat_control = NULL;
	}

	if (u->dbus) {
		dbus_g_connection_unref(u->dbus);
	}

	ret = -1;
finish:
	if (busProxy)
		g_object_unref (busProxy);

	return ret;
}

