/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009-2012 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <colord.h>
#include <math.h>
#include <stdlib.h>
#include <lcms2.h>
#include <colorhug.h>
#include <canberra-gtk.h>

#include "ch-cell-renderer-date.h"
#include "ch-cell-renderer-postage.h"
#include "ch-cell-renderer-uint32.h"
#include "ch-database.h"
#include "ch-shipping-common.h"

typedef struct {
	GSettings	*settings;
	GtkApplication	*application;
	GtkBuilder	*builder;
	ChDatabase	*database;
	GString		*output_csv;
	guint		 invoices[CH_SHIPPING_POSTAGE_LAST];
	GMainLoop	*loop;
} ChFactoryPrivate;

enum {
	COLUMN_ID,
	COLUMN_NAME,
	COLUMN_ADDRESS,
	COLUMN_EMAIL,
	COLUMN_FILENAME,
	COLUMN_TRACKING,
	COLUMN_SHIPPED,
	COLUMN_POSTAGE,
	COLUMN_DEVICE_ID,
	COLUMN_LAST
};

#define CH_ORDER_ICON_NEW		"dialog-information"
#define CH_ORDER_ICON_SENT		"colorimeter-colorhug"
#define CH_ORDER_ICON_PRINTED		"printer"

/**
 * ch_shipping_error_dialog:
 **/
static void
ch_shipping_error_dialog (ChFactoryPrivate *priv,
			  const gchar *title,
			  const gchar *message)
{
	GtkWindow *window;
	GtkWidget *dialog;

	window = GTK_WINDOW(gtk_builder_get_object (priv->builder, "dialog_shipping"));
	dialog = gtk_message_dialog_new (window,
					 GTK_DIALOG_MODAL,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_CLOSE,
					 "%s", title);
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						  "%s", message);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}

/**
 * ch_shipping_activate_cb:
 **/
static void
ch_shipping_activate_cb (GApplication *application, ChFactoryPrivate *priv)
{
	GtkWindow *window;
	window = GTK_WINDOW (gtk_builder_get_object (priv->builder, "dialog_shipping"));
	gtk_window_present (window);
}

/**
 * ch_shipping_close_button_cb:
 **/
static void
ch_shipping_close_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_shipping"));
	gtk_widget_destroy (widget);
}

/**
 * ch_shipping_find_by_id:
 **/
static gboolean
ch_shipping_find_by_id (GtkTreeModel *model,
		        GtkTreeIter *iter_found,
		        guint order_id)
{
	gboolean ret;
	GtkTreeIter iter;
	guint order_id_tmp;

	ret = gtk_tree_model_get_iter_first (model, &iter);
	while (ret) {
		gtk_tree_model_get (model, &iter,
				    COLUMN_ID, &order_id_tmp,
				    -1);
		if (order_id_tmp == order_id) {
			*iter_found = iter;
			break;
		}
		ret = gtk_tree_model_iter_next (model, &iter);
	}
	return ret;
}

/**
 * ch_shipping_set_order_state:
 **/
static void
ch_shipping_set_order_state (ChFactoryPrivate *priv,
			      guint order_id,
			      const gchar *state)
{
	gboolean ret;
	GtkListStore *list_store;
	GtkTreeIter iter;

	list_store = GTK_LIST_STORE (gtk_builder_get_object (priv->builder, "liststore_orders"));
	ret = ch_shipping_find_by_id (GTK_TREE_MODEL (list_store),
				     &iter,
				     order_id);
	if (!ret)
		return;
	gtk_list_store_set (list_store, &iter,
			    COLUMN_FILENAME, state,
			    -1);
}

/**
 * ch_shipping_refresh_status:
 **/
static void
ch_shipping_refresh_status (ChFactoryPrivate *priv)
{
	GError *error = NULL;
	GtkWidget *widget;
	guint cnt = 0;
	guint devices_left;
	guint queue_size = 0;
	guint i;
	GPtrArray *queue = NULL;

	/* update status */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "label_status"));
	devices_left = ch_database_device_get_number (priv->database, CH_DATABASE_STATE_CALIBRATED, &error);
	if (devices_left == G_MAXUINT) {
		ch_shipping_error_dialog (priv, "Failed to get number of devices", error->message);
		g_error_free (error);
		goto out;
	}

	/* count pending print jobs */
	for (i = 0; i < priv->output_csv->len; i++) {
		if (priv->output_csv->str[i] == '\n')
			cnt++;
	}

	/* count preorder list size */
	queue = ch_database_get_queue (priv->database, &error);
	if (queue == NULL) {
		ch_shipping_error_dialog (priv, "Failed to get preorder queue", error->message);
		g_error_free (error);
		goto out;
	}
	queue_size = queue->len;
out:
	if (queue != NULL)
		g_ptr_array_unref (queue);
	gtk_label_set_text (GTK_LABEL (widget), g_strdup_printf ("%i devices remaining to be sold, %i preorders, %i documents waiting to be printed", devices_left, queue_size, cnt));
}

/**
 * ch_shipping_refresh_orders:
 **/
static void
ch_shipping_refresh_orders (ChFactoryPrivate *priv)
{
	ChDatabaseOrder *order;
	const gchar *icon;
	gboolean ret;
	GError *error = NULL;
	GPtrArray *array;
	GtkListStore *list_store;
	GtkTreeIter iter;
	guint32 device_id;
	guint i;

	list_store = GTK_LIST_STORE (gtk_builder_get_object (priv->builder, "liststore_orders"));
	array = ch_database_get_all_orders (priv->database, &error);
	if (array == NULL) {
		ch_shipping_error_dialog (priv, "Failed to get all orders", error->message);
		g_error_free (error);
		goto out;
	}
	for (i = 0; i < array->len; i++) {
		order = g_ptr_array_index (array, i);
		ret = ch_shipping_find_by_id (GTK_TREE_MODEL (list_store), &iter, order->order_id);
		if (!ret)
			gtk_list_store_append (list_store, &iter);

		if (order->sent_date == 0)
			icon = CH_ORDER_ICON_NEW;
		else
			icon = CH_ORDER_ICON_SENT;

		/* get the device ID if we can */
		device_id = ch_database_order_get_device_id (priv->database,
							     order->order_id,
							     NULL);
		if (device_id == G_MAXUINT32)
			device_id = 0;

		gtk_list_store_set (list_store, &iter,
				    COLUMN_FILENAME, icon,
				    COLUMN_ID, order->order_id,
				    COLUMN_NAME, order->name,
				    COLUMN_EMAIL, order->email,
				    COLUMN_ADDRESS, order->address,
				    COLUMN_TRACKING, order->tracking_number,
				    COLUMN_SHIPPED, order->sent_date,
				    COLUMN_POSTAGE, order->postage,
				    COLUMN_DEVICE_ID, device_id,
				    -1);
	}

	/* and also status */
	ch_shipping_refresh_status (priv);
out:
	if (array != NULL)
		g_ptr_array_unref (array);
}

/**
 * ch_shipping_queue_button_cb:
 **/
static void
ch_shipping_queue_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	ChShippingPostage postage;
	gboolean ret;
	gchar *address = NULL;
	gchar *name = NULL;
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	GtkTreeView *treeview;
	guint32 device_id;
	guint32 order_id;

	/* get selected item */
	treeview = GTK_TREE_VIEW (gtk_builder_get_object (priv->builder, "treeview_orders"));
	selection = gtk_tree_view_get_selection (treeview);
	ret = gtk_tree_selection_get_selected (selection, &model, &iter);
	if (!ret)
		goto out;
	gtk_tree_model_get (model, &iter,
			    COLUMN_ID, &order_id,
			    COLUMN_NAME, &name,
			    COLUMN_ADDRESS, &address,
			    COLUMN_POSTAGE, &postage,
			    COLUMN_DEVICE_ID, &device_id,
			    -1);

	/* update icon */
	ch_shipping_set_order_state (priv, order_id, CH_ORDER_ICON_PRINTED);

	/* add to invoide total */
	priv->invoices[postage]++;

	/* add to XML string in the format:
	 * name,addr1,addr2,addr3,addr4,addr5,serial,shipping,cost */
	g_string_append_printf (priv->output_csv, "\"%s\",%s,%04i,%s,%i\n",
				name,
				address,
				device_id,
				ch_shipping_postage_to_string (postage),
				0);
	g_warning ("queue now %s", priv->output_csv->str);
	ch_shipping_refresh_status (priv);
out:
	g_free (address);
	g_free (name);
}

/**
 * ch_shipping_get_invoice_filename:
 **/
static const gchar *
ch_shipping_get_invoice_filename (ChShippingPostage postage)
{
	if (postage == CH_SHIPPING_POSTAGE_UNKNOWN)
		return "Invoice Freebe.odt";
	if (postage == CH_SHIPPING_POSTAGE_UK)
		return "Invoice UK £50.odt";
	if (postage == CH_SHIPPING_POSTAGE_EUROPE)
		return "Invoice Europe £51.odt";
	if (postage == CH_SHIPPING_POSTAGE_WORLD)
		return "Invoice World £52.odt";
	if (postage == CH_SHIPPING_POSTAGE_UK_SIGNED)
		return "Invoice UK £55 (signed).odt";
	if (postage == CH_SHIPPING_POSTAGE_EUROPE_SIGNED)
		return "Invoice Europe £56 (signed).odt";
	if (postage == CH_SHIPPING_POSTAGE_WORLD_SIGNED)
		return "Invoice World £57 (signed).odt";
	return NULL;
}

/**
 * ch_shipping_print_invoices_button_cb:
 **/
static void
ch_shipping_print_invoices_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	gboolean ret;
	gchar *cmd;
	gchar *document_path;
	GError *error = NULL;
	guint i, j;

	document_path = g_settings_get_string (priv->settings, "invoice-documents-uri");
	for (i = 0; i < CH_SHIPPING_POSTAGE_LAST; i++) {
		ch_shipping_get_invoice_filename (i);
		for (j = 0; j < priv->invoices[i]; j++) {
			cmd = g_strdup_printf ("soffice -p '%s/%s'", document_path, ch_shipping_get_invoice_filename (i));
			ret = g_spawn_command_line_sync (cmd, NULL, NULL, NULL, &error);
			if (!ret) {
				ch_shipping_error_dialog (priv, "Failed to print invoice", error->message);
				g_error_free (error);
				goto out;
			}
			g_free (cmd);
		}
	}

	/* reset the invoice count */
	for (i = 0; i < CH_SHIPPING_POSTAGE_LAST; i++)
		priv->invoices[i] = 0;

	/* refresh status */
	ch_shipping_refresh_status (priv);
out:
	g_free (document_path);
}

/**
 * ch_shipping_print_labels_button_cb:
 **/
static void
ch_shipping_print_labels_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	gboolean ret;
	gchar *cmd = NULL;
	gchar *labels_data = NULL;
	gchar *labels_template = NULL;
	GError *error = NULL;

	/* add the output header */
	g_string_prepend (priv->output_csv,
			  "name,address1,address2,address3,address4,address5,serial,shipping,cost\n");

	/* dump the print data to a file */
	labels_data = g_settings_get_string (priv->settings, "labels-data");
	g_debug ("Writing to: %s", labels_data);
	ret = g_file_set_contents (labels_data,
				   priv->output_csv->str,
				   -1,
				   &error);
	if (!ret) {
		ch_shipping_error_dialog (priv, "Failed to write CSV file", error->message);
		g_error_free (error);
		goto out;
	}

	/* create the pdf */
	labels_template = g_settings_get_string (priv->settings, "labels-template");
	cmd = g_strdup_printf ("glabels-3-batch --output=/tmp/colorhug.pdf '%s'", labels_template);
	g_debug ("Running: %s", cmd);
	ret = g_spawn_command_line_sync (cmd,
					 NULL,
					 NULL,
					 NULL,
					 &error);
	if (!ret) {
		ch_shipping_error_dialog (priv, "Failed to create labels file", error->message);
		g_error_free (error);
		goto out;
	}

	/* print the PDF */
	ret = g_spawn_command_line_async ("evince /tmp/colorhug.pdf", &error);
	if (!ret) {
		ch_shipping_error_dialog (priv, "Failed to view pdf", error->message);
		g_error_free (error);
		goto out;
	}

	/* clear the pending queue of things to print */
	g_string_set_size (priv->output_csv, 0);

	/* refresh status */
	ch_shipping_refresh_status (priv);
out:
	g_free (cmd);
	g_free (labels_template);
	g_free (labels_data);
}

/**
 * ch_shipping_order_button_cb:
 **/
static void
ch_shipping_order_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_order_add"));
	gtk_widget_set_sensitive (widget, FALSE);

	/* set to defaults */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_name"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_email"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr1"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr2"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr3"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr4"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr5"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");

	/* show the modal window */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_order"));
	gtk_widget_show (widget);
}

/**
 * ch_shipping_order_cancel_button_cb:
 **/
static void
ch_shipping_order_cancel_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_order"));
	gtk_widget_hide (widget);
}

/**
 * ch_shipping_invite_cancel_button_cb:
 **/
static void
ch_shipping_invite_cancel_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	if (g_main_loop_is_running (priv->loop))
		g_main_loop_quit (priv->loop);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_invite"));
	gtk_widget_hide (widget);
}

/**
 * ch_shipping_queue_cancel_button_cb:
 **/
static void
ch_shipping_queue_cancel_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_queue"));
	gtk_widget_hide (widget);
}

/**
 * ch_shipping_invite_button_cb:
 **/
static void
ch_shipping_invite_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	/* clear existing */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "progressbar_invite"));
	gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (widget), 0.0);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "spinbutton_invite_delay"));
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (widget), 2.0);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_invite"));
	gtk_widget_show (widget);
}

/**
 * ch_shipping_preorder_button_cb:
 **/
static void
ch_shipping_preorder_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	/* clear existing */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_queue_emails"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_queue"));
	gtk_widget_show (widget);
}

/**
 * ch_shipping_shipped_button_cb:
 **/
static void
ch_shipping_shipped_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	ChShippingPostage postage;
	gboolean ret;
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	GtkTreeView *treeview;

	/* get the order id of the selected item */
	treeview = GTK_TREE_VIEW (gtk_builder_get_object (priv->builder, "treeview_orders"));
	selection = gtk_tree_view_get_selection (treeview);
	ret = gtk_tree_selection_get_selected (selection, &model, &iter);
	if (!ret)
		goto out;
	gtk_tree_model_get (model, &iter,
			    COLUMN_POSTAGE, &postage,
			    -1);

	/* clear existing */
	if (postage == CH_SHIPPING_POSTAGE_UK ||
	    postage == CH_SHIPPING_POSTAGE_EUROPE ||
	    postage == CH_SHIPPING_POSTAGE_WORLD) {
		widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_shipped_tracking"));
		gtk_entry_set_text (GTK_ENTRY (widget), "n/a");
		gtk_widget_set_sensitive (widget, FALSE);

		/* setup state */
		widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_shipped_email"));
		gtk_widget_set_sensitive (widget, TRUE);
	} else {
		widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_shipped_tracking"));
		gtk_entry_set_text (GTK_ENTRY (widget), "");
		gtk_widget_set_sensitive (widget, TRUE);

		/* setup state */
		widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_shipped_email"));
		gtk_widget_set_sensitive (widget, FALSE);
	}
out:
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_shipped"));
	gtk_widget_show (widget);
}

/**
 * ch_shipping_shipped_cancel_button_cb:
 **/
static void
ch_shipping_shipped_cancel_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_shipped"));
	gtk_widget_hide (widget);
}

/**
 * ch_shipping_loop_quit_cb:
 **/
static gboolean
ch_shipping_loop_quit_cb (gpointer user_data)
{
	ChFactoryPrivate *priv = (ChFactoryPrivate *) user_data;
	g_main_loop_quit (priv->loop);
	return FALSE;
}

/**
 * ch_shipping_parse_email_list:
 **/
static GPtrArray *
ch_shipping_parse_email_list (const gchar *value)
{
	gchar **split = NULL;
	gchar *tmp = NULL;
	GPtrArray *array;
	guint i;

	/* get the actual email addresses from the junk we just pasted in */
	array = g_ptr_array_new_with_free_func (g_free);
	tmp = g_strdup (value);
	g_strdelimit  (tmp, "<>\n✆;,", ' ');
	split = g_strsplit (tmp, " ", -1);
	for (i = 0; split[i] != NULL; i++) {
		if (split[i][0] == '\0')
			continue;
		if (g_strstr_len (split[i], -1, "@") == NULL)
			continue;
		if (g_strstr_len (split[i], -1, ".") == NULL)
			continue;
		g_ptr_array_add (array, g_strdup (split[i]));
		g_usleep (10);
	}
	g_strfreev (split);
	g_free (tmp);
	return array;
}

/**
 * ch_shipping_queue_add_button_cb:
 **/
static void
ch_shipping_queue_add_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	const gchar *recipient;
	const guint delay_ms = 1000;
	gboolean promote;
	gboolean ret;
	gchar *sender = NULL;
	GError *error = NULL;
	GPtrArray *array;
	GString *str = NULL;
	guint i;
	guint queue_id;

	/* get the actual email addresses from the junk we just pasted in */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_queue_emails"));
	array = ch_shipping_parse_email_list (gtk_entry_get_text (GTK_ENTRY (widget)));

	/* create the email */
	str = g_string_new ("");

	g_string_append (str, "Just a quick note to say we've acknowledged your preorder request for a ColorHug.\n\n");
	g_string_append (str, "We're now making about 50 devices a week so it shouldn't be long until I email you "
			      "to tell you that your device is built, tested and ready to send.\n\n");
	g_string_append (str, "Thanks again for your patience and support.\n\n");
	g_string_append (str, "Many thanks,\n\n");
	g_string_append (str, "Richard Hughes");

	/* get promotion status */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "checkbutton_queue_promote"));
	promote = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));

	/* add each email */
	sender = g_settings_get_string (priv->settings, "invoice-sender");
	for (i = 0; i < array->len; i++) {
		recipient = g_ptr_array_index (array, i);
		g_debug ("recipient=%s", recipient);
		queue_id = ch_database_queue_add (priv->database,
					          recipient,
					          &error);
		if (queue_id == G_MAXUINT) {
			ch_shipping_error_dialog (priv, "Failed to add email", error->message);
			g_clear_error (&error);
			continue;
		}

		/* promote? */
		if (promote) {
			ret = ch_database_queue_promote (priv->database,
							 recipient,
							 &error);
			if (!ret) {
				ch_shipping_error_dialog (priv, "Failed to promote email", error->message);
				g_clear_error (&error);
				continue;
			}
		}

		/* send email */
		ret = ch_shipping_send_email (sender,
					      recipient,
					      "Confirming your ColorHug pre-order",
					      str->str,
					      &error);
		if (!ret) {
			ch_shipping_error_dialog (priv, "Failed to send email", error->message);
			g_clear_error (&error);
			continue;
		}

		/* wait the required delay, unless it's the last item */
		if (i + 1 < array->len) {
			g_timeout_add (delay_ms + g_random_int_range (0, delay_ms),
				       ch_shipping_loop_quit_cb, priv);
			g_main_loop_run (priv->loop);
		}
	}

	/* refresh status */
	ch_shipping_refresh_status (priv);

	/* clear text box and close window */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_queue_emails"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_queue"));
	gtk_widget_hide (widget);
//out:
	g_free (sender);
	g_ptr_array_unref (array);
	if (str != NULL)
		g_string_free (str, TRUE);
}

/**
 * ch_shipping_invite_send_button_cb:
 **/
static void
ch_shipping_invite_send_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	const gchar *recipient;
	gboolean ret;
	gchar *sender = NULL;
	GError *error = NULL;
	GPtrArray *array;
	GString *str = NULL;
	guint delay_ms;
	guint invite_number;
	guint i;

	/* get email delay */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "spinbutton_invite_delay"));
	delay_ms = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (widget)) * 1000;

	/* get number of people to email */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "spinbutton_invite_number"));
	invite_number = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (widget));

	/* get the actual email addresses from the junk we just pasted in */
	array = ch_database_get_queue (priv->database, &error);
	if (array == NULL) {
		ch_shipping_error_dialog (priv, "Failed to get preorder queue", error->message);
		g_error_free (error);
		goto out;
	}

	/* create the email */
	str = g_string_new ("");
	g_string_append (str, "I am pleased to announce that the third batch of ColorHugs are now built, ");
	g_string_append (str, "tested and ready to ship.\n\n");
	g_string_append (str, "You are now being given an opportunity to purchase one ColorHug. ");
	g_string_append (str, "If you don't want to buy a ColorHug just yet, just let me know ");
	g_string_append (str, "and I'll keep you on the list and offer the hardware from the second batch to someone else.\n\n");
	g_string_append (str, "Please read the details on the link below *carefully* before you purchase a ColorHug. ");
	g_string_append (str, "Please don't share the URL as there are only 100 units in this batch.\n\n");
	g_string_append (str, "http://www.hughski.com/buy-from-preorder.html\n\n");
	g_string_append (str, "I would ask you either decline the offer or complete the purchase within 7 days, ");
	g_string_append (str, "otherwise I'll have to allocate your ColorHug to somebody else.\n\n");
	g_string_append (str, "Thanks again for your support for this new and exciting project.\n\n");
	g_string_append (str, "Many thanks,\n\n");
	g_string_append (str, "Richard Hughes");

	/* send each email */
	sender = g_settings_get_string (priv->settings, "invoice-sender");
	for (i = 0; i < array->len && i < invite_number; i++) {
		recipient = g_ptr_array_index (array, i);
		g_debug ("recipient=%s", recipient);

		/* remove from db */
		ret = ch_database_queue_remove (priv->database,
						recipient,
						&error);
		if (!ret) {
			ch_shipping_error_dialog (priv, "Failed to remove email", error->message);
			g_clear_error (&error);
			continue;
		}

		ret = ch_shipping_send_email (sender,
					      recipient,
					      "Your ColorHug is now in stock!",
					      str->str,
					      &error);
		if (!ret) {
			ch_shipping_error_dialog (priv, "Failed to send email", error->message);
			g_error_free (error);
			goto out;
		}

		/* wait the required delay */
		g_timeout_add (delay_ms + g_random_int_range (0, delay_ms),
			       ch_shipping_loop_quit_cb, priv);
		g_main_loop_run (priv->loop);

		/* update progress */
		widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "progressbar_invite"));
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (widget), (gdouble) i / (gdouble) array->len);
	}

	/* refresh status */
	ch_shipping_refresh_status (priv);

	/* play sound */
	ca_context_play (ca_gtk_context_get (), 0,
			 CA_PROP_EVENT_ID, "alarm-clock-elapsed",
			 CA_PROP_APPLICATION_NAME, _("ColorHug Factory"),
			 CA_PROP_EVENT_DESCRIPTION, _("Email sending completed"), NULL);

	/* clear text box and close window */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_invite"));
	gtk_widget_hide (widget);
out:
	if (str != NULL)
		g_string_free (str, TRUE);
	if (array != NULL)
		g_ptr_array_unref (array);
	g_ptr_array_unref (array);
	g_free (sender);
}

/**
 * ch_shipping_order_add_button_cb:
 **/
static void
ch_shipping_order_add_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	ChShippingPostage postage = CH_SHIPPING_POSTAGE_LAST;
	const gchar *email = NULL;
	const gchar *name = NULL;
	gboolean ret;
	GError *error = NULL;
	GString *addr = g_string_new ("");
	guint32 device_id;
	guint32 order_id;

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_paypal"));
	name = gtk_entry_get_text (GTK_ENTRY (widget));
	if (name != NULL && name[0] != '\0') {
		/* import data */
		goto skip;
	}

	/* get name */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_name"));
	name = gtk_entry_get_text (GTK_ENTRY (widget));

	/* get email */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_email"));
	email = gtk_entry_get_text (GTK_ENTRY (widget));

	/* add address */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr1"));
	g_string_append_printf (addr, "\"%s\",", gtk_entry_get_text (GTK_ENTRY (widget)));
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr2"));
	g_string_append_printf (addr, "\"%s\",", gtk_entry_get_text (GTK_ENTRY (widget)));
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr3"));
	g_string_append_printf (addr, "\"%s\",", gtk_entry_get_text (GTK_ENTRY (widget)));
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr4"));
	g_string_append_printf (addr, "\"%s\",", gtk_entry_get_text (GTK_ENTRY (widget)));
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr5"));
	g_string_append_printf (addr, "\"%s\"", gtk_entry_get_text (GTK_ENTRY (widget)));

	/* get postage */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping1"));
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
		postage = CH_SHIPPING_POSTAGE_UK;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping2"));
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
		postage = CH_SHIPPING_POSTAGE_EUROPE;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping3"));
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
		postage = CH_SHIPPING_POSTAGE_WORLD;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping4"));
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
		postage = CH_SHIPPING_POSTAGE_UK_SIGNED;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping5"));
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
		postage = CH_SHIPPING_POSTAGE_EUROPE_SIGNED;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping6"));
	if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
		postage = CH_SHIPPING_POSTAGE_WORLD_SIGNED;
skip:
	/* get the oldest device we've got calibrated */
	device_id = ch_database_device_find_oldest (priv->database,
						    CH_DATABASE_STATE_CALIBRATED,
						    &error);
	if (device_id == G_MAXUINT32) {
		ch_shipping_error_dialog (priv, "Failed to get device", error->message);
		g_error_free (error);
		goto out;
	}

	/* add to database */
	order_id = ch_database_add_order (priv->database, name, addr->str, email, postage, &error);
	if (order_id == G_MAXUINT32) {
		ch_shipping_error_dialog (priv, "Failed to add order", error->message);
		g_error_free (error);
		goto out;
	}

	/* add this device to the order */
	ret = ch_database_device_set_order_id (priv->database,
					       device_id,
					       order_id,
					       &error);
	if (!ret) {
		ch_shipping_error_dialog (priv, "Failed to assign device to order", error->message);
		g_error_free (error);
		goto out;
	}

	/* mark the device as allocated */
	ret = ch_database_device_set_state (priv->database,
					    device_id,
					    CH_DATABASE_STATE_ALLOCATED,
					    &error);
	if (!ret) {
		ch_shipping_error_dialog (priv, "Failed to allocate device", error->message);
		g_error_free (error);
		goto out;
	}

	/* refresh state */
	ch_shipping_refresh_orders (priv);
out:
	/* buttons */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_order"));
	gtk_widget_hide (widget);
}

/**
 * ch_shipping_shipped_email_button_cb:
 **/
static void
ch_shipping_shipped_email_button_cb (GtkWidget *widget, ChFactoryPrivate *priv)
{
	ChShippingPostage postage;
	const gchar *tracking_number = NULL;
	gboolean ret;
	gchar *cmd = NULL;
	gchar *email = NULL;
	gchar *from = NULL;
	GError *error = NULL;
	GString *str = NULL;
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	GtkTreeView *treeview;
	guint32 order_id;
	guint32 device_id;

	/* get the order id of the selected item */
	treeview = GTK_TREE_VIEW (gtk_builder_get_object (priv->builder, "treeview_orders"));
	selection = gtk_tree_view_get_selection (treeview);
	ret = gtk_tree_selection_get_selected (selection, &model, &iter);
	if (!ret)
		goto out;
	gtk_tree_model_get (model, &iter,
			    COLUMN_ID, &order_id,
			    COLUMN_POSTAGE, &postage,
			    COLUMN_EMAIL, &email,
			    COLUMN_DEVICE_ID, &device_id,
			    -1);

	/* get tracking number */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_shipped_tracking"));
	tracking_number = gtk_entry_get_text (GTK_ENTRY (widget));

	/* write email */
	str = g_string_new ("");
	from = g_settings_get_string (priv->settings, "invoice-sender");
	if (tracking_number[0] == '\0' || g_strcmp0 (tracking_number, "n/a") == 0) {
		g_string_append_printf (str, "I'm pleased to tell you ColorHug #%04i has been dispatched.\n", device_id);
	} else {
		g_string_append_printf (str, "I'm pleased to tell you ColorHug #%04i has been dispatched with tracking ID: %s\n", device_id, tracking_number);
		g_string_append (str, "You will be able to track this item here: http://track2.royalmail.com/portal/rm/trackresults\n");
	}
	g_string_append (str, "\n");
	if (postage == CH_SHIPPING_POSTAGE_UK || postage == CH_SHIPPING_POSTAGE_UK_SIGNED) {
		g_string_append (str, "Please allow up to two weeks for delivery, although most items are delivered within 4 days.\n");
	} else if (postage == CH_SHIPPING_POSTAGE_EUROPE || postage == CH_SHIPPING_POSTAGE_EUROPE_SIGNED) {
		g_string_append (str, "Please allow up to two weeks for delivery, although most items are delivered within 8 days.\n");
	} else if (postage == CH_SHIPPING_POSTAGE_WORLD || postage == CH_SHIPPING_POSTAGE_WORLD_SIGNED) {
		g_string_append (str, "Please allow up to three weeks for delivery, although most items are delivered within 10 days.\n");
	}
	g_string_append (str, "\n");
	g_string_append (str, "Thanks again for your support for this new and exciting project.\n");
	g_string_append (str, "\n");
	g_string_append (str, "Richard Hughes\n");

	/* actually send the email */
	ret = ch_shipping_send_email (from,
				      email,
				      "Your ColorHug has been dispatched!",
				      str->str,
				      &error);
	if (!ret) {
		ch_shipping_error_dialog (priv, "Failed to send email", error->message);
		g_error_free (error);
		goto out;
	}

	/* save to the database */
	ret = ch_database_order_set_tracking (priv->database,
					      order_id,
					      tracking_number,
					      &error);
	if (!ret) {
		ch_shipping_error_dialog (priv, "Failed to mark order as shipped", error->message);
		g_error_free (error);
		goto out;
	}

	/* refresh state */
	ch_shipping_refresh_orders (priv);
out:
	g_free (from);
	g_free (email);
	g_free (cmd);
	if (str != NULL)
		g_string_free (str, TRUE);
	/* buttons */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_shipped"));
	gtk_widget_hide (widget);
}

/**
 * ch_shipping_row_activated_cb:
 **/
static void
ch_shipping_row_activated_cb (GtkTreeView *treeview,
			      GtkTreePath *path,
			      GtkTreeViewColumn *col,
			      ChFactoryPrivate *priv)
{
	gboolean ret;
	GtkTreeIter iter;
	GtkTreeModel *model;
	guint32 id = 0;

	/* get selection */
	model = gtk_tree_view_get_model (treeview);
	ret = gtk_tree_model_get_iter (model, &iter, path);
	if (!ret) {
		g_warning ("failed to get selection");
		goto out;
	}

	/* get data */
	gtk_tree_model_get (model, &iter,
			    COLUMN_ID, &id,
			    -1);
	if (id == G_MAXUINT32)
		goto out;
	g_debug ("activated: %i", id);
out:
	return;
}

/**
 * ch_shipping_treeview_add_columns:
 **/
static void
ch_shipping_treeview_add_columns (ChFactoryPrivate *priv)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeView *treeview;

	treeview = GTK_TREE_VIEW (gtk_builder_get_object (priv->builder, "treeview_orders"));
	g_signal_connect (treeview, "row-activated",
			  G_CALLBACK (ch_shipping_row_activated_cb), priv);

	/* column for images */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_pixbuf_new ();
	g_object_set (renderer, "stock-size", GTK_ICON_SIZE_BUTTON, NULL);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "icon-name", COLUMN_FILENAME);
	gtk_tree_view_column_set_title (column, "Status");
	gtk_tree_view_append_column (treeview, column);

	/* column for name */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "markup", COLUMN_NAME);
	gtk_tree_view_column_set_title (column, "Name");
	gtk_tree_view_append_column (treeview, column);

	/* column for tracking */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "markup", COLUMN_TRACKING);
	gtk_tree_view_column_set_title (column, "Tracking");
	gtk_tree_view_append_column (treeview, column);

	/* column for shipped */
	column = gtk_tree_view_column_new ();
	renderer = ch_cell_renderer_date_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "value", COLUMN_SHIPPED);
	gtk_tree_view_column_set_title (column, "Shipped");
	gtk_tree_view_append_column (treeview, column);

	/* column for postage */
	column = gtk_tree_view_column_new ();
	renderer = ch_cell_renderer_postage_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "value", COLUMN_POSTAGE);
	gtk_tree_view_column_set_title (column, "Postage");
	gtk_tree_view_append_column (treeview, column);

	/* column for device_id */
	column = gtk_tree_view_column_new ();
	renderer = ch_cell_renderer_uint32_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_add_attribute (column, renderer, "value", COLUMN_DEVICE_ID);
	gtk_tree_view_column_set_title (column, "Device Serial");
	gtk_tree_view_append_column (treeview, column);
	gtk_tree_view_column_set_sort_column_id (column, COLUMN_DEVICE_ID);
}

/**
 * ch_shipping_paypal_entry_changed_cb:
 **/
static void
ch_shipping_paypal_entry_changed_cb (GtkWidget *widget, GParamSpec *param_spec, ChFactoryPrivate *priv)
{
	const gchar *value;
	gboolean is_address = FALSE;
	gchar **lines = NULL;
	guint cnt = 0;
	guint i;

	/* get import text */
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;
	lines = g_strsplit (value, "\n", -1);

	/* parse block of text */
	for (i = 0; lines[i] != NULL; i++) {
		value = lines[i];

		/* get email */
		if (!g_str_has_prefix (value, "You received a payment of") &&
		    g_strstr_len (value, -1, "@") &&
		    !g_str_has_prefix (value, "Payment sent to ")) {
			widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_email"));
			gtk_entry_set_text (GTK_ENTRY (widget), value);
		}

		/* get postage */
		if (g_strstr_len (value, -1, "Postage to ")) {
			if (g_strstr_len (value, -1, "UK (signed for)")) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping4"));
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
			} else if (g_strstr_len (value, -1, "Europe (signed for)")) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping5"));
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
			} else if (g_strstr_len (value, -1, "rest of the world (signed for)")) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping6"));
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
			} else if (g_strstr_len (value, -1, "UK")) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping1"));
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
			} else if (g_strstr_len (value, -1, "Europe")) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping2"));
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
			} else if (g_strstr_len (value, -1, "rest of the world")) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "radiobutton_shipping3"));
				gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
			} else {
				g_warning ("no postage %s", value);
				g_assert_not_reached ();
			}
		}

		if (g_str_has_prefix (value, "Send-to address") ||
		    g_str_has_prefix (value, "Postal address") ||
		    g_str_has_prefix (value, "Shipping address")) {
			is_address = TRUE;
			continue;
		}
		if (is_address) {
			if (value[0] == '\t') {
				is_address = FALSE;
				continue;
			}
			if (cnt == 0) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_name"));
				gtk_entry_set_text (GTK_ENTRY (widget), value);
			} else if (cnt == 1) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr1"));
				gtk_entry_set_text (GTK_ENTRY (widget), value);
			} else if (cnt == 2) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr2"));
				gtk_entry_set_text (GTK_ENTRY (widget), value);
			} else if (cnt == 3) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr3"));
				gtk_entry_set_text (GTK_ENTRY (widget), value);
			} else if (cnt == 4) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr4"));
				gtk_entry_set_text (GTK_ENTRY (widget), value);
			} else if (cnt == 5) {
				widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr5"));
				gtk_entry_set_text (GTK_ENTRY (widget), value);
			} else {
				g_warning ("address[%i] = %s", cnt, value);
				g_assert_not_reached ();
			}
			cnt++;
		}
	}

	/* clear this */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_paypal"));
	gtk_entry_set_text (GTK_ENTRY (widget), "");
out:
	g_strfreev (lines);
}

/**
 * ch_shipping_order_entry_changed_cb:
 **/
static void
ch_shipping_order_entry_changed_cb (GtkWidget *widget, GParamSpec *param_spec, ChFactoryPrivate *priv)
{
	const gchar *value;
	gboolean ret = FALSE;

	/* set to defaults */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_name"));
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_email"));
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr1"));
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr2"));
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr3"));
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr4"));
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr5"));
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;

	/* woohoo */
	ret = TRUE;
out:
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_order_add"));
	gtk_widget_set_sensitive (widget, ret);
}

/**
 * ch_shipping_shipping_entry_changed_cb:
 **/
static void
ch_shipping_shipping_entry_changed_cb (GtkWidget *widget, GParamSpec *param_spec, ChFactoryPrivate *priv)
{
	const gchar *value;
	gboolean ret = FALSE;

	/* set to defaults */
	value = gtk_entry_get_text (GTK_ENTRY (widget));
	if (value[0] == '\0')
		goto out;

	/* woohoo */
	ret = TRUE;
out:
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_shipped_email"));
	gtk_widget_set_sensitive (widget, ret);
}


/**
 * ch_shipping_treeview_clicked_cb:
 **/
static void
ch_shipping_treeview_clicked_cb (GtkTreeSelection *selection, ChFactoryPrivate *priv)
{
	gboolean ret;
	gchar *tracking_number = NULL;
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkWidget *widget;

	/* get the order id of the selected item */
	ret = gtk_tree_selection_get_selected (selection, &model, &iter);
	if (!ret)
		goto out;
	gtk_tree_model_get (model, &iter,
			    COLUMN_TRACKING, &tracking_number,
			    -1);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "toolbutton_shipped"));
	gtk_widget_set_sensitive (widget, tracking_number[0] == '\0');
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "toolbutton_queue"));
	gtk_widget_set_sensitive (widget, tracking_number[0] == '\0');
out:
	/* buttons */
	g_free (tracking_number);
}

/**
 * ch_shipping_startup_cb:
 **/
static void
ch_shipping_startup_cb (GApplication *application, ChFactoryPrivate *priv)
{
	gchar *filename = NULL;
	GError *error = NULL;
	gint retval;
	GtkTreeSelection *selection;
	GtkWidget *main_window;
	GtkWidget *widget;
	GtkStyleContext *context;
	GtkTreeSortable *sortable;

	/* get UI */
	priv->builder = gtk_builder_new ();
	retval = gtk_builder_add_from_file (priv->builder,
					    CH_DATA "/ch-shipping.ui",
					    &error);
	if (retval == 0) {
		g_warning ("failed to load ui: %s",
			   error->message);
		g_error_free (error);
		goto out;
	}

	/* this is the output csv */
	priv->output_csv = g_string_new ("");

	/* add application specific icons to search path */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   CH_DATA G_DIR_SEPARATOR_S "icons");

	main_window = GTK_WIDGET (gtk_builder_get_object (priv->builder, "dialog_shipping"));
	gtk_application_add_window (priv->application, GTK_WINDOW (main_window));
	gtk_widget_set_size_request (main_window, 600, 400);

	/* Hide window first so that the dialogue resizes itself without redrawing */
	gtk_widget_hide (main_window);

	/* setup treeview */
	ch_shipping_treeview_add_columns (priv);

	/* sorted */
	sortable = GTK_TREE_SORTABLE (gtk_builder_get_object (priv->builder, "liststore_orders"));
	gtk_tree_sortable_set_sort_column_id (sortable,
					      COLUMN_DEVICE_ID, GTK_SORT_DESCENDING);

	/* buttons */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_close"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_close_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_print_labels"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_print_labels_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_print_invoices"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_print_invoices_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "toolbutton_queue"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_queue_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_order"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_order_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "toolbutton_shipped"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_shipped_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_invite"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_invite_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_preorder"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_preorder_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_order_cancel"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_order_cancel_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_order_add"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_order_add_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_shipped_cancel"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_shipped_cancel_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_shipped_email"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_shipped_email_button_cb), priv);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_invite_cancel"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_invite_cancel_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_invite_send"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_invite_send_button_cb), priv);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_queue_cancel"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_queue_cancel_button_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "button_queue_add"));
	g_signal_connect (widget, "clicked",
			  G_CALLBACK (ch_shipping_queue_add_button_cb), priv);

	/* don't allow to add without details */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_name"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_order_entry_changed_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr1"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_order_entry_changed_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr2"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_order_entry_changed_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr3"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_order_entry_changed_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr4"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_order_entry_changed_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_addr5"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_order_entry_changed_cb), priv);
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_email"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_order_entry_changed_cb), priv);

	/* parse the paypal email */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_paypal"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_paypal_entry_changed_cb), priv);

	/* don't allow to send without tracking number */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "entry_shipped_tracking"));
	g_signal_connect (widget, "notify::text",
			  G_CALLBACK (ch_shipping_shipping_entry_changed_cb), priv);

	/* disable buttons based on the order state */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "treeview_orders"));
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
	g_signal_connect (selection, "changed",
			  G_CALLBACK (ch_shipping_treeview_clicked_cb), priv);

	/* make devices toolbar sexy */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder,
						     "scrolledwindow_devices"));
	context = gtk_widget_get_style_context (widget);
	gtk_style_context_set_junction_sides (context, GTK_JUNCTION_BOTTOM);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder,
						     "toolbar_devices"));
	context = gtk_widget_get_style_context (widget);
	gtk_style_context_add_class (context, GTK_STYLE_CLASS_INLINE_TOOLBAR);
	gtk_style_context_set_junction_sides (context, GTK_JUNCTION_TOP);

	/* coldplug from database */
	ch_shipping_refresh_orders (priv);

	/* show main UI */
	gtk_widget_show (main_window);
out:
	g_free (filename);
}

/**
 * ch_shipping_ignore_cb:
 **/
static void
ch_shipping_ignore_cb (const gchar *log_domain, GLogLevelFlags log_level,
		      const gchar *message, gpointer user_data)
{
}

/**
 * main:
 **/
int
main (int argc, char **argv)
{
	ChFactoryPrivate *priv;
	gboolean ret;
	gboolean verbose = FALSE;
	GError *error = NULL;
	GOptionContext *context;
	guint i;
	int status = 0;
	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			/* TRANSLATORS: command line option */
			_("Show extra debugging information"), NULL },
		{ NULL}
	};

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	/* TRANSLATORS: A program to shipping calibrate the hardware */
	context = g_option_context_new (_("ColorHug CCMX loader"));
	g_option_context_add_group (context, gtk_get_option_group (TRUE));
	g_option_context_add_main_entries (context, options, NULL);
	ret = g_option_context_parse (context, &argc, &argv, &error);
	if (!ret) {
		g_warning ("%s: %s",
			   _("Failed to parse command line options"),
			   error->message);
		g_error_free (error);
	}
	g_option_context_free (context);

	priv = g_new0 (ChFactoryPrivate, 1);
	priv->loop = g_main_loop_new (NULL, FALSE);
	priv->database = ch_database_new ();
	priv->settings = g_settings_new ("com.hughski.colorhug-tools");

	/* reset the invoice count */
	for (i = 0; i < CH_SHIPPING_POSTAGE_LAST; i++)
		priv->invoices[i] = 0;

	/* ensure single instance */
	priv->application = gtk_application_new ("com.hughski.ColorHug.Shipping", 0);
	g_signal_connect (priv->application, "startup",
			  G_CALLBACK (ch_shipping_startup_cb), priv);
	g_signal_connect (priv->application, "activate",
			  G_CALLBACK (ch_shipping_activate_cb), priv);
	/* set verbose? */
	if (verbose) {
		g_setenv ("COLORHUG_VERBOSE", "1", FALSE);
	} else {
		g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				   ch_shipping_ignore_cb, NULL);
	}

	/* wait */
	status = g_application_run (G_APPLICATION (priv->application), argc, argv);

	g_main_loop_unref (priv->loop);
	g_object_unref (priv->application);
	if (priv->output_csv != NULL)
		g_string_free (priv->output_csv, TRUE);
	if (priv->builder != NULL)
		g_object_unref (priv->builder);
	if (priv->settings != NULL)
		g_object_unref (priv->settings);
	if (priv->database != NULL)
		g_object_unref (priv->database);
	g_free (priv);
	return status;
}