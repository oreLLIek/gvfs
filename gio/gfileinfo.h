#ifndef __G_FILE_INFO_H__
#define __G_FILE_INFO_H__

#include <sys/stat.h>

#include <glib-object.h>
#include <gio/gvfstypes.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_INFO         (g_file_info_get_type ())
#define G_FILE_INFO(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_INFO, GFileInfo))
#define G_FILE_INFO_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_INFO, GFileInfoClass))
#define G_IS_FILE_INFO(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_INFO))
#define G_IS_FILE_INFO_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_INFO))
#define G_FILE_INFO_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_INFO, GFileInfoClass))

typedef struct _GFileInfo        GFileInfo;
typedef struct _GFileInfoClass   GFileInfoClass;
typedef struct _GFileInfoPrivate GFileInfoPrivate;
typedef struct _GFileAttribute   GFileAttribute;
typedef struct _GFileAttributeMatcher GFileAttributeMatcher;

typedef enum {
  G_FILE_TYPE_UNKNOWN = 0,
  G_FILE_TYPE_REGULAR,
  G_FILE_TYPE_DIRECTORY,
  G_FILE_TYPE_SYMBOLIC_LINK,
  G_FILE_TYPE_SPECIAL, /* socket, fifo, blockdev, chardev */
  G_FILE_TYPE_SHORTCUT,
  G_FILE_TYPE_MOUNTABLE
} GFileType;

typedef enum {
  G_FILE_ACCESS_CAN_READ = 1 << 1,
  G_FILE_ACCESS_CAN_WRITE = 1 << 2,
  G_FILE_ACCESS_CAN_EXECUTE = 1 << 3,
  G_FILE_ACCESS_CAN_DELETE = 1 << 4,
  G_FILE_ACCESS_CAN_RENAME = 1 << 5,
} GFileAccessRights;

struct _GFileInfo
{
  GObject parent_instance;

  GFileInfoPrivate *priv;
};

struct _GFileInfoClass
{
  GObjectClass parent_class;
};

struct _GFileAttribute {
  const char *attribute;
  const char *value;
};

typedef enum {
  G_FILE_INFO_FILE_TYPE         = 1 << 0,
  G_FILE_INFO_NAME              = 1 << 1,
  G_FILE_INFO_DISPLAY_NAME      = 1 << 2,
  G_FILE_INFO_EDIT_NAME         = 1 << 3,
  G_FILE_INFO_ICON              = 1 << 4,
  G_FILE_INFO_MIME_TYPE         = 1 << 5,
  G_FILE_INFO_SIZE              = 1 << 6,
  G_FILE_INFO_MODIFICATION_TIME = 1 << 7,
  G_FILE_INFO_ACCESS_RIGHTS     = 1 << 8,
  G_FILE_INFO_STAT_INFO         = 1 << 9,
  G_FILE_INFO_SYMLINK_TARGET    = 1 << 10,
  G_FILE_INFO_IS_HIDDEN         = 1 << 11,
} GFileInfoRequestFlags;

#define G_FILE_INFO_REQUEST_FLAGS_FROM_STAT_MASK (G_FILE_INFO_FILE_TYPE | G_FILE_INFO_SIZE | G_FILE_INFO_MODIFICATION_TIME | G_FILE_INFO_STAT_INFO) 

GType g_file_info_get_type (void) G_GNUC_CONST;
  
GFileInfo *            g_file_info_new                   (void);
GFileType              g_file_info_get_file_type         (GFileInfo         *info);
const char *           g_file_info_get_name              (GFileInfo         *info);
const char *           g_file_info_get_display_name      (GFileInfo         *info);
const char *           g_file_info_get_edit_name         (GFileInfo         *info);
const char *           g_file_info_get_icon              (GFileInfo         *info);
const char *           g_file_info_get_mime_type         (GFileInfo         *info);
GQuark                 g_file_info_get_mime_type_quark   (GFileInfo         *info);
goffset                g_file_info_get_size              (GFileInfo         *info);
time_t                 g_file_info_get_modification_time (GFileInfo         *info);
const char *           g_file_info_get_symlink_target    (GFileInfo         *info);
GFileAccessRights      g_file_info_get_access_rights     (GFileInfo         *info);
gboolean               g_file_info_can_read              (GFileInfo         *info);
gboolean               g_file_info_can_write             (GFileInfo         *info);
gboolean               g_file_info_can_delete            (GFileInfo         *info);
gboolean               g_file_info_can_rename            (GFileInfo         *info);
gboolean               g_file_info_get_is_hidden         (GFileInfo         *info);
const struct stat *    g_file_info_get_stat_info         (GFileInfo         *info);
const char *           g_file_info_get_attribute         (GFileInfo         *info,
							  const char        *attribute);
GFileAttribute *       g_file_info_get_attributes        (GFileInfo         *info,
							  const char        *namespace,
							  int               *n_attributes);
GFileAttribute *       g_file_info_get_all_attributes    (GFileInfo         *info,
							  int               *n_attributes);
void                   g_file_info_set_file_type         (GFileInfo         *info,
							  GFileType          type);
void                   g_file_info_set_name              (GFileInfo         *info,
							  const char        *name);
void                   g_file_info_set_display_name      (GFileInfo         *info,
							  const char        *display_name);
void                   g_file_info_set_edit_name         (GFileInfo         *info,
							  const char        *edit_name);
void                   g_file_info_set_icon              (GFileInfo         *info,
							  const char        *icon);
void                   g_file_info_set_mime_type         (GFileInfo         *info,
							  const char        *mime_type);
void                   g_file_info_set_size              (GFileInfo         *info,
							  goffset            size);
void                   g_file_info_set_modification_time (GFileInfo         *info,
							  time_t             time);
void                   g_file_info_set_symlink_target    (GFileInfo         *info,
							  const char        *link_target);
void                   g_file_info_set_access_rights     (GFileInfo         *info,
							  GFileAccessRights  access_rights);
void                   g_file_info_set_is_hidden         (GFileInfo         *info,
							  gboolean           is_hidden);
void                   g_file_info_set_stat_info         (GFileInfo         *info,
							  const struct stat *statbuf);
void                   g_file_info_set_attribute         (GFileInfo         *info,
							  const char        *attribute,
							  const char        *value);
void                   g_file_info_set_attributes        (GFileInfo         *info,
							  GFileAttribute    *attributes,
							  int                n_attributes);

void                   g_file_info_set_from_stat         (GFileInfo         *info,
							  GFileInfoRequestFlags requested,
							  const struct stat *statbuf);

GFileAttributeMatcher *g_file_attribute_matcher_new            (const char            *attributes);
void                   g_file_attribute_matcher_free           (GFileAttributeMatcher *matcher);
gboolean               g_file_attribute_matcher_matches        (GFileAttributeMatcher *matcher,
								const char            *namespace,
								const char            *full_name);
gboolean               g_file_attribute_matcher_matches_q      (GFileAttributeMatcher *matcher,
								GQuark                 namespace,
								GQuark                 full_name);
gboolean               g_file_attribute_matcher_enumerate      (GFileAttributeMatcher *matcher,
								const char            *namespace);
gboolean               g_file_attribute_matcher_enumerate_q    (GFileAttributeMatcher *matcher,
								GQuark                 namespace);
const char *           g_file_attribute_matcher_enumerate_next (GFileAttributeMatcher *matcher);

G_END_DECLS


#endif /* __G_FILE_INFO_H__ */