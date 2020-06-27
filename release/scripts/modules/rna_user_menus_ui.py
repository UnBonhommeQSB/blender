# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>

__all__ = (
    "draw_user_menus",
)

import bpy
from bpy.app.translations import pgettext_iface as iface_
from bpy.app.translations import contexts as i18n_contexts


def draw_item(context, box, but_list):
    prefs = context.preferences
    um = prefs.user_menus

    for items in but_list:
        name = items.item.name
        if items.item.type == "SEPARATOR":
            name = "___________"
        box.prop(items, "pressed", text=name, toggle=1)
        if items.item.type == "SUBMENU":
            sub_box = box.box()
            sub_box = sub_box.column(align=True)
            draw_item(context, sub_box, items.subbut)


def draw_item_box(context, row):
    prefs = context.preferences
    um = prefs.user_menus

    box_line = row.box()
    box_col = box_line.column(align=True)

    has_item = um.has_item()
    if not has_item:
        box_col.label(text="none")
    um.buttons_refresh()
    draw_item(context, box_col, um.buttons)

    row = row.split(factor=0.9, align=True)
    col = row.column(align=True)

    col.operator("preferences.menuitem_add", text="", icon='ADD')
    col.operator("preferences.menuitem_remove", text="", icon='REMOVE')
    col.operator("preferences.menuitem_up", text="", icon='TRIA_UP')
    col.operator("preferences.menuitem_down", text="", icon='TRIA_DOWN')
    row.separator()


def draw_item_editor(context, row):
    prefs = context.preferences
    um = prefs.user_menus

    col = row.column()

    has_item = um.has_item()
    current = um.active_item
    if not has_item:
        col.label(text="No item in this list.")
        col.label(text="Add one or choose another list to get started")
    elif current:
        col.prop(current, "type")
        if (current.type != "SEPARATOR"):
            col.prop(current, "name")
        if (current.type == "OPERATOR"):
            umi_op = current.get_operator()
            col.prop(umi_op, "operator")
        if (current.type == "MENU"):
            umi_pm = current.get_menu()
            col.prop(umi_pm, "id_name", text="ID name")
    else:
        col.label(text="No item selected.")


def draw_user_menus(context, layout):
    prefs = context.preferences
    um = prefs.user_menus

    menu_name_active = None
    if not menu_name_active:
        menu_name_active = "Quick favourites"

    split = layout.split(factor=0.4)

    row = split.row()
    col = layout.column()

    rowsub = row.row(align=True)
    rowsub.menu("USERPREF_MT_menu_select", text=menu_name_active)
    rowsub.operator("wm.keyconfig_preset_add", text="", icon='ADD')
    rowsub.operator("wm.keyconfig_preset_add", text="",
                    icon='REMOVE').remove_active = True

    rowsub = split.row(align=True)
    rowsub.prop(um, "space_selected", text="")

    rowsub = split.row(align=True)
    rowsub.prop(um, "context_selected", text="")

    rowsub = split.row(align=True)
    rowsub.operator("preferences.keyconfig_import", text="", icon='IMPORT')
    rowsub.operator("preferences.keyconfig_export", text="", icon='EXPORT')

    row = layout.row()
    col = layout.column()
    rowsub = row.split(factor=0.4, align=True)

    layout.separator()
    # TODO : set menu parameters in a submenu here

    col = layout.column()
    row = layout.row()

    draw_item_box(context=context, row=row)
    draw_item_editor(context=context, row=row)

    layout.separator()