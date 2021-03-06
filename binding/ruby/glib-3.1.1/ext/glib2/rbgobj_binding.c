/* -*- c-file-style: "ruby"; indent-tabs-mode: nil -*- */
/*
 *  Copyright (C) 2015  Ruby-GNOME2 Project Team
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "rbgprivate.h"

#define RG_TARGET_NAMESPACE cBinding

#define _SELF(object) G_BINDING(RVAL2GOBJ(self))

#if GLIB_CHECK_VERSION(2, 38, 0)
static VALUE
rg_unbind(VALUE self)
{
    g_binding_unbind(_SELF(self));
    return self;
}
#endif

void
Init_gobject_gbinding(void)
{
#if GLIB_CHECK_VERSION(2, 26, 0)
    VALUE RG_TARGET_NAMESPACE;

    RG_TARGET_NAMESPACE = G_DEF_CLASS(G_TYPE_BINDING, "Binding", mGLib);
#endif

#if GLIB_CHECK_VERSION(2, 38, 0)
    RG_DEF_METHOD(unbind, 0);
#endif
}
