# pyspatialite-working

Fixes for https://code.google.com/p/pyspatialite/ which appears to be inactive since at least Feb 2013.

Based almost entirely on pysqlite, this module primarily does one thing different: it build's it's own copy of sqlite with spatialite built in.

It should be easy to to integrate into SQLAlchemy, and can be used as a drop in replacement for pysqlite anywhere - it just has some extra SQL functions.

libgeos, libgeos_c, and libproj are required to build currently. sets include and lib paths (if not standard) in setup.cfg

For OS X, get the GEOS and PROJ Frameworks from http://www.kyngchaos.com/software:frameworks

If you're trying to use this with SqlAlchemy, see: UsingWithSqlAlchemy
