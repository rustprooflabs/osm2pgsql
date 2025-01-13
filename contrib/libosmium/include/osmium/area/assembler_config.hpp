#ifndef OSMIUM_AREA_ASSEMBLER_CONFIG_HPP
#define OSMIUM_AREA_ASSEMBLER_CONFIG_HPP

/*

This file is part of Osmium (https://osmcode.org/libosmium).

Copyright 2013-2025 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

namespace osmium {

    namespace area {

        class ProblemReporter;

        /**
         * Configuration for osmium::area::Assembler objects. Create this
         * once, set the options you want and then re-use it every time you
         * create an Assembler object.
         */
        struct AssemblerConfig {

            /**
             * Optional pointer to problem reporter.
             */
            ProblemReporter* problem_reporter = nullptr;

            /**
             * Debug level. If this is greater than zero, debug messages will
             * be printed to stderr. Available levels are 1 to 3. Note that
             * level 2 and above will generate a lot of messages!
             */
            int debug_level = 0;

            /**
             * The roles of multipolygon members are ignored when assembling
             * multipolygons, because they are often missing or wrong. If this
             * is set, the roles are checked after the multipolygons are built
             * against what the assembly process decided where the inner and
             * outer rings are. This slows down the processing, so it only
             * makes sense if you want to get the problem reports.
             */
            bool check_roles = false;

            /**
             * When the assembler can't create an area, usually because its
             * geometry would be invalid, it will create an "empty" area object
             * without rings. This allows you to detect where an area was
             * invalid.
             *
             * If this is set to false, invalid areas will simply be discarded.
             */
            bool create_empty_areas = true;

            /**
             * Create areas for (multi)polygons where the tags are on the
             * relation.
             *
             * If this is set to false, those areas will simply be discarded.
             */
            bool create_new_style_polygons = true;

            /**
             * Create areas for (multi)polygons where the tags are on the
             * outer way(s). This is ignored by the area::Assembler which
             * doesn't support old-style multipolygons any more. Use the
             * area::AssemblerLegacy if you need this.
             *
             * If this is set to false, those areas will simply be discarded.
             */
            bool create_old_style_polygons = true;

            /**
             * Create areas for polygons created from ways.
             *
             * If this is set to false, those areas will simply be discarded.
             */
            bool create_way_polygons = true;

            /**
             * Keep the type tag from multipolygon relations on the area
             * object. By default this is false, and the type tag will be
             * removed.
             */
            bool keep_type_tag = false;

            /**
             * If there is an invalid location in any of the ways needed for
             * assembling the multipolygon, the assembler will normally fail.
             * If this is set, the assembler will silently ignore the invalid
             * locations pretending them to be not referenced from the ways.
             * This will allow some areas to be built, others will now be
             * incorrect. This can sometimes be useful to assemble areas
             * crossing the boundary of an extract, but you will also get
             * geometrically valid but wrong (multi)polygons.
             */
            bool ignore_invalid_locations = false;

            AssemblerConfig() noexcept = default;

        }; // struct AssemblerConfig

    } // namespace area

} // namespace osmium

#endif // OSMIUM_AREA_ASSEMBLER_CONFIG_HPP
