/*
 * Copyright 2009 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/**
 * SECTION:nbtk-style
 * @short_description: a data store for style properties
 *
 * #NbtkStyle is a property data store that can read properties from a style
 * sheet. It is queried with objects that implement the NbtkStylable
 * interface.
 */


#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <glib-object.h>
#include <gobject/gvaluecollector.h>
#include <glib/gi18n-lib.h>

#include <clutter/clutter.h>

#include <ccss/ccss.h>

#include "nbtk-stylable.h"
#include "nbtk-style.h"
#include "nbtk-types.h"
#include "nbtk-marshal.h"
#include "nbtk-widget.h"

enum
{
  CHANGED,

  LAST_SIGNAL
};

#define NBTK_STYLE_GET_PRIVATE(obj) \
        (G_TYPE_INSTANCE_GET_PRIVATE ((obj), NBTK_TYPE_STYLE, NbtkStylePrivate))

#define NBTK_STYLE_ERROR g_style_error_quark ()

typedef struct {
  GType value_type;
  gchar *value_name;
  GValue value;
} StyleProperty;

struct _NbtkStylePrivate
{
  ccss_stylesheet_t *stylesheet;
  GList *image_paths;

  GHashTable *style_hash;
  GHashTable *node_hash;
};

typedef struct {
  ccss_node_t parent;
  NbtkStylable *stylable;
  NbtkStylableIface *iface;
} nbtk_style_node_t;

static ccss_function_t const * peek_css_functions (void);

static ccss_node_class_t * peek_node_class (void);

static guint style_signals[LAST_SIGNAL] = { 0, };

static NbtkStyle *default_style = NULL;

G_DEFINE_TYPE (NbtkStyle, nbtk_style, G_TYPE_OBJECT);

static GQuark
g_style_error_quark (void)
{
  return g_quark_from_static_string ("nbtk-style-error-quark");
}

static gboolean
nbtk_style_real_load_from_file (NbtkStyle    *style,
                                const gchar  *filename,
                                GError      **error,
                                gint          priority)
{
  NbtkStylePrivate *priv;
  ccss_grammar_t *grammar;
  GError *internal_error;
  gchar *path;
  GList *l;

  g_return_val_if_fail (NBTK_IS_STYLE (style), FALSE);
  g_return_val_if_fail (filename != NULL, FALSE);

  priv = NBTK_STYLE (style)->priv;

  if (!g_file_test (filename, G_FILE_TEST_IS_REGULAR))
    {
      internal_error = g_error_new (NBTK_STYLE_ERROR,
                                    NBTK_STYLE_ERROR_INVALID_FILE,
                                    _("Invalid theme file '%s'"), filename);
      g_propagate_error (error, internal_error);
      return FALSE;
    }


  /* add the path of the stylesheet to the search path */
  path = g_path_get_dirname (filename);

  /* make sure path is valid */
  if (!path)
    return TRUE;

  for (l = priv->image_paths; l; l = l->next)
    {
      if (g_str_equal ((gchar *)l->data, path))
        {
          /* we have this path already */
          g_free (path);
          path = NULL;
        }
    }

  /* Add the new path */
  if (path)
    priv->image_paths = g_list_append (priv->image_paths, path);

  /* now load the stylesheet */
  if (!priv->stylesheet)
    {
      grammar = ccss_grammar_create_css ();
      ccss_grammar_add_functions (grammar, peek_css_functions ());
      priv->stylesheet = ccss_grammar_create_stylesheet_from_file (grammar,
                                                                   filename,
                                                                   path);
      ccss_grammar_destroy (grammar);
    }
  else
    {
      ccss_stylesheet_add_from_file (priv->stylesheet, filename, priority, path);
    }

  g_signal_emit (style, style_signals[CHANGED], 0, NULL);

  return TRUE;
}

/**
 * nbtk_style_load_from_file:
 * @style: a #NbtkStyle
 * @filename: filename of the style sheet to load
 * @error: a #GError or #NULL
 *
 * Load style information from the specified file.
 *
 * returns: TRUE if the style information was loaded successfully. Returns
 * FALSE on error.
 */
gboolean
nbtk_style_load_from_file (NbtkStyle    *style,
                           const gchar  *filename,
                           GError      **error)
{
  return nbtk_style_real_load_from_file (style, filename, error,
                                         CCSS_STYLESHEET_AUTHOR);
}

static void
nbtk_style_finalize (GObject *gobject)
{
  NbtkStylePrivate *priv = ((NbtkStyle *)gobject)->priv;
  GList *l;

  for (l = priv->image_paths; l; l = g_list_delete_link (l, l))
  {
    g_free (l->data);
  }

  G_OBJECT_CLASS (nbtk_style_parent_class)->finalize (gobject);
}

static void
nbtk_style_class_init (NbtkStyleClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (NbtkStylePrivate));

  gobject_class->finalize = nbtk_style_finalize;

  /**
   * NbtkStyle::changed:
   *
   * Indicates that the style data has changed in some way. For example, a new
   * stylesheet may have been loaded.
   */

  style_signals[CHANGED] =
    g_signal_new ("changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (NbtkStyleClass, changed),
                  NULL, NULL,
                  _nbtk_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);
}

/* url loader for libccss */
static char *
ccss_url (GSList const  *args,
          void          *user_data)
{
  const gchar *given_path, *filename;
  gchar *test_path;

  g_return_val_if_fail (args, NULL);

  given_path = (char const *) args->data;

  /* we can only deal with local paths */
  if (!g_str_has_prefix (given_path, "file://"))
    return NULL;
  filename = &given_path[7];

  /*
   * Handle absolute paths correctly
   */
  if (*filename == '/')
    return strdup (filename);

  /* first try looking in the theme dir */
  test_path = g_build_filename (g_get_user_config_dir (),
                                "nbtk",
                                filename,
                                NULL);
  if (g_file_test (test_path, G_FILE_TEST_IS_REGULAR))
    return test_path;
  g_free (test_path);

  if (user_data)
    {
      test_path = g_build_filename ((gchar *) user_data, filename, NULL);

      if (g_file_test (test_path, G_FILE_TEST_IS_REGULAR))
        return test_path;

      g_free (test_path);
    }
  else
    {
      g_warning ("No path available css url resolver!");
    }

  /* couldn't find the image anywhere, so just return the filename */
  return strdup (given_path);
}

static ccss_function_t const *
peek_css_functions (void)
{
  static ccss_function_t const ccss_functions[] =
  {
    { "url", ccss_url },
    { NULL }
  };

  return ccss_functions;
}


static void
nbtk_style_init (NbtkStyle *style)
{
  NbtkStylePrivate *priv;

  style->priv = priv = NBTK_STYLE_GET_PRIVATE (style);

  /* create a hash table to look up pointer keys and values */
  style->priv->node_hash = g_hash_table_new_full (NULL, NULL,
                                                  NULL, g_free);
  style->priv->style_hash = g_hash_table_new_full (NULL, NULL,
                                                   NULL, (GDestroyNotify) ccss_style_destroy);

}

/**
 * nbtk_style_new:
 *
 * Creates a new #NbtkStyle object. This must be freed using #g_object_unref
 * when no longer required.
 *
 * Returns: a newly allocated #NbtkStyle
 */
NbtkStyle *
nbtk_style_new (void)
{
  return g_object_new (NBTK_TYPE_STYLE, NULL);
}

/**
 * nbtk_style_get_default:
 *
 * Return the default NbtkStyle object. This includes the current theme (if
 * any).
 *
 * Returns: (transfer none): a #NbtkStyle object. This must not be freed or
 * unref'd by applications
 */
NbtkStyle *
nbtk_style_get_default (void)
{
  if (G_LIKELY (default_style))
    return default_style;

  default_style = g_object_new (NBTK_TYPE_STYLE, NULL);

  return default_style;
}

/* functions for ccss */

static nbtk_style_node_t *
get_container (nbtk_style_node_t *node)
{
  nbtk_style_node_t *container;
  ClutterActor      *parent;

  g_return_val_if_fail (node, NULL);
  g_return_val_if_fail (node->iface, NULL);
  g_return_val_if_fail (node->stylable, NULL);

  parent = clutter_actor_get_parent (CLUTTER_ACTOR (node->stylable));
  while (parent && !NBTK_IS_WIDGET (parent))
    parent = clutter_actor_get_parent (CLUTTER_ACTOR (parent));

  if (!parent)
    return NULL;

  container = g_new0 (nbtk_style_node_t, 1);
  ccss_node_init ((ccss_node_t*) container, peek_node_class ());
  container->iface = node->iface;
  container->stylable = NBTK_STYLABLE (parent);

  return container;
}

static const gchar*
get_style_id (nbtk_style_node_t *node)
{
  return nbtk_stylable_get_style_id (node->stylable);
}

static const gchar*
get_style_type (nbtk_style_node_t *node)
{
  return nbtk_stylable_get_style_type (node->stylable);
}

static const gchar*
get_style_class (nbtk_style_node_t *node)
{
  return nbtk_stylable_get_style_class (node->stylable);
}

static const gchar*
get_pseudo_class (nbtk_style_node_t *node)
{
  return nbtk_stylable_get_pseudo_class (node->stylable);
}

static const gchar*
get_attribute (nbtk_style_node_t *node, const char *name)
{
  return nbtk_stylable_get_attribute (node->stylable, name);
}

static void
release (nbtk_style_node_t *node)
{
  g_return_if_fail (node);

  g_free (node);
}

static ccss_node_class_t *
peek_node_class (void)
{
  static ccss_node_class_t _node_class = {
    .is_a             = NULL,
    .get_container    = (ccss_node_get_container_f) get_container,
    .get_id           = (ccss_node_get_id_f) get_style_id,
    .get_type         = (ccss_node_get_type_f) get_style_type,
    .get_class        = (ccss_node_get_class_f) get_style_class,
    .get_pseudo_class = (ccss_node_get_pseudo_class_f) get_pseudo_class,
    .get_viewport     = NULL,// (ccss_node_get_viewport_f) get_viewport,
    .get_attribute    = (ccss_node_get_attribute_f) get_attribute,
    .release          = (ccss_node_release_f) release
  };

  return &_node_class;
}

static void
nbtk_style_fetch_ccss_property (ccss_style_t  *ccss_style,
                                GParamSpec    *pspec,
                                GValue        *value)
{
  gboolean value_set = FALSE;

  g_value_init (value, G_PARAM_SPEC_VALUE_TYPE (pspec));

  if (G_PARAM_SPEC_VALUE_TYPE (pspec))
    {
      double number;

      if (G_IS_PARAM_SPEC_INT (pspec))
        {
          if (ccss_style_get_double (ccss_style, pspec->name, &number))
            {
              g_value_set_int (value, (gint) number);
              value_set = TRUE;
            }
        }
      else if (G_IS_PARAM_SPEC_UINT (pspec))
        {
          if (ccss_style_get_double (ccss_style, pspec->name, &number))
            {
              g_value_set_uint (value, (guint) number);
              value_set = TRUE;
            }
        }
      else if (G_PARAM_SPEC_VALUE_TYPE (pspec) == NBTK_TYPE_BORDER_IMAGE &&
               !g_strcmp0 ("border-image", pspec->name))
        {
              ccss_border_image_t const *border_image;

          if (ccss_style_get_property (ccss_style,
                                        "border-image",
                                        (ccss_property_base_t const **) &border_image))
            {
              if (border_image &&
                  border_image->base.state == CCSS_PROPERTY_STATE_SET)
                {
                  g_value_set_boxed (value, border_image);
                  value_set = TRUE;
                }
            }
        }
      else if (NBTK_TYPE_PADDING == G_PARAM_SPEC_VALUE_TYPE (pspec) &&
                0 == g_strcmp0 ("padding", pspec->name))
        {
          NbtkPadding padding = { 0, };
          gboolean padding_set = 0;

          if (ccss_style_get_double (ccss_style, "padding-top", &number))
            {
              padding.top = number;
              padding_set = TRUE;
            }

          if (ccss_style_get_double (ccss_style, "padding-right", &number))
            {
              padding.right = number;
              padding_set = TRUE;
            }

          if (ccss_style_get_double (ccss_style, "padding-bottom", &number))
            {
              padding.bottom = number;
              padding_set = TRUE;
            }

          if (ccss_style_get_double (ccss_style, "padding-left", &number))
            {
              padding.left = number;
              padding_set = TRUE;
            }

          if (padding_set)
            {
              g_value_set_boxed (value, &padding);
              value_set = TRUE;
            }
        }
      else
        {
          gchar *string = NULL;

          ccss_style_get_string (ccss_style, pspec->name, &string);

          if (string)
            {
              if (CLUTTER_IS_PARAM_SPEC_COLOR (pspec))
                {
                  ClutterColor color = { 0, };

                  clutter_color_from_string (&color, string);
                  clutter_value_set_color (value, &color);

                  value_set = TRUE;
                }
              else
                if (G_IS_PARAM_SPEC_STRING (pspec))
                  {
                    g_value_set_string (value, string);
                    value_set = TRUE;
                  }
              g_free (string);
            }
        }
    }

  /* no value was found in css, so copy in the default value */
  if (!value_set)
    g_param_value_set_default (pspec, value);
}

static ccss_style_t*
nbtk_style_get_ccss_query (NbtkStyle         *style,
                           NbtkStylable      *stylable)
{
  NbtkStylableIface *iface = NBTK_STYLABLE_GET_IFACE (stylable);
  ccss_style_t *ccss_style;
  nbtk_style_node_t *ccss_node;

  ccss_node = g_hash_table_lookup (style->priv->node_hash, stylable);

  if (!ccss_node)
    {
      ccss_node = g_new0 (nbtk_style_node_t, 1);
      ccss_node_init ((ccss_node_t*) ccss_node, peek_node_class ());
      ccss_node->iface = iface;
      ccss_node->stylable = stylable;

      g_hash_table_insert (style->priv->node_hash, stylable, ccss_node);
      g_signal_connect_swapped (stylable, "stylable-changed",
                                G_CALLBACK (g_hash_table_remove),
                                style->priv->node_hash);


      g_object_weak_ref ((GObject*) stylable,
                        (GWeakNotify) g_hash_table_remove, style->priv->node_hash);
    }


  ccss_style = g_hash_table_lookup (style->priv->style_hash, stylable);

  if (!ccss_style)
    {
      ccss_style = ccss_stylesheet_query (style->priv->stylesheet,
                                          (ccss_node_t *) ccss_node);

      g_hash_table_insert (style->priv->style_hash, stylable, ccss_style);

      /* remove the cache if the stylable changes */
      g_signal_connect_swapped (stylable, "stylable-changed",
                                G_CALLBACK (g_hash_table_remove),
                                style->priv->style_hash);

      g_object_weak_ref ((GObject*) stylable,
                         (GWeakNotify) g_hash_table_remove, style->priv->style_hash);
    }

  return ccss_style;

}


/**
 * nbtk_style_get_property:
 * @style: the style data store object
 * @stylable: a stylable to retreive the data for
 * @pspec: a #GParamSpec describing the property required
 * @value: (out): a #GValue to place the return value in
 *
 * Requests the property described in @pspec for the specified stylable
 */

void
nbtk_style_get_property (NbtkStyle    *style,
                         NbtkStylable *stylable,
                         GParamSpec   *pspec,
                         GValue       *value)
{
  NbtkStylePrivate *priv;
  gboolean value_set = FALSE;

  g_return_if_fail (NBTK_IS_STYLE (style));
  g_return_if_fail (NBTK_IS_STYLABLE (stylable));
  g_return_if_fail (pspec != NULL);
  g_return_if_fail (value != NULL);

  priv = style->priv;

  /* look up the property in the css */
  if (priv->stylesheet)
    {
      ccss_style_t *ccss_style;

      ccss_style = nbtk_style_get_ccss_query (style, stylable);
      if (ccss_style)
        {
          nbtk_style_fetch_ccss_property (ccss_style, pspec, value);
          value_set = TRUE;
        }
    }

  /* no value was found in css, so copy in the default value */
  if (!value_set)
    {
      g_value_init (value, G_PARAM_SPEC_VALUE_TYPE (pspec));
      g_param_value_set_default (pspec, value);
    }
}

/**
 * nbtk_style_get_valist:
 * @style: a #NbtkStyle
 * @stylable: a #NbtkStylable
 * @first_property_name: name of the first property to get
 * @va_args: return location for the first property, followed optionally
 *   by more name/return location pairs, followed by %NULL
 *
 * Gets the style properties for @stylable from @style.
 *
 * Please refer to nbtk_style_get() for further information.
 */
void
nbtk_style_get_valist (NbtkStyle     *style,
                       NbtkStylable  *stylable,
                       const gchar   *first_property_name,
                       va_list        va_args)
{
  NbtkStylePrivate *priv;
  const gchar *name = first_property_name;
  gboolean values_set = FALSE;

  g_return_if_fail (NBTK_IS_STYLE (style));
  g_return_if_fail (NBTK_IS_STYLABLE (stylable));
  g_return_if_fail (style->priv != NULL);

  priv = style->priv;

  /* look up the property in the css */
  if (priv->stylesheet)
    {
      ccss_style_t *ccss_style;

      ccss_style = nbtk_style_get_ccss_query (style, stylable);

      if (ccss_style)
        {
          while (name)
            {
              GValue value = { 0, };
              gchar *error = NULL;
              GParamSpec *pspec = nbtk_stylable_find_property (stylable, name);
              nbtk_style_fetch_ccss_property (ccss_style, pspec, &value);
              G_VALUE_LCOPY (&value, va_args, 0, &error);
              if (error)
                {
                  g_warning ("%s: %s", G_STRLOC, error);
                  g_free (error);
                  g_value_unset (&value);
                  break;
                }
              g_value_unset (&value);
              name = va_arg (va_args, gchar*);
            }
          values_set = TRUE;
        }
    }

  if (!values_set)
    {
      /* Set the remaining properties to their default values
       * even if broken out of the above loop. */
      while (name)
        {
          GValue value = { 0, };
          gchar *error = NULL;
          nbtk_stylable_get_default_value (stylable, name, &value);
          G_VALUE_LCOPY (&value, va_args, 0, &error);
          if (error)
            {
              g_warning ("%s: %s", G_STRLOC, error);
              g_free (error);
              g_value_unset (&value);
              break;
            }
          g_value_unset (&value);
          name = va_arg (va_args, gchar*);
        }
    }
}

/**
 * nbtk_style_get:
 * @style: a #NbtkStyle
 * @stylable: a #NbtkStylable
 * @first_property_name: name of the first property to get
 * @Varargs: return location for the first property, followed optionally
 *   by more name/return location pairs, followed by %NULL
 *
 * Gets the style properties for @stylable from @style.
 *
 * In general, a copy is made of the property contents and the caller
 * is responsible for freeing the memory in the appropriate manner for
 * the property type.
 */
void
nbtk_style_get (NbtkStyle     *style,
                NbtkStylable  *stylable,
                const gchar   *first_property_name,
                ...)
{
  va_list va_args;

  g_return_if_fail (NBTK_IS_STYLE (style));
  g_return_if_fail (first_property_name != NULL);

  va_start (va_args, first_property_name);
  nbtk_style_get_valist (style, stylable, first_property_name, va_args);
  va_end (va_args);
}
