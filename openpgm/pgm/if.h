/* vim:ts=8:sts=4:sw=4:noai:noexpandtab
 * 
 * network interface handling.
 *
 * Copyright (c) 2006 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __PGM_IF_H__
#define __PGM_IF_H__


G_BEGIN_DECLS

int if_print_all (void);
int if_parse_network (const char*);


G_END_DECLS

#endif /* __PGM_IF_H__ */