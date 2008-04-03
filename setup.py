from distutils.core import setup
from distutils.extension import Extension
from Cython.Distutils import build_ext

long_description = \
"""Python interface to SQLite 3

pysqlite is an interface to the SQLite 3.x embedded relational database engine.
It is almost fully compliant with the Python database API version 2.0."""

def get_setup_args():
    py_modules = ["sqlite"]

    ext = Extension(
        "_pysqlite4",
        ["sqlite.pyx"]
    )

    setup_args = dict(
            name = "pysqlite",
            version = "4.0.alpha1",
            description = "DB-API 2.0 interface for SQLite 3.x",
            long_description=long_description,
            author = "Gerhard Haering",
            author_email = "gh@ghaering.de",
            license = "zlib/libpng license",
            platforms = "ALL",
            url = "http://pysqlite.org/",
            # download_url = "..."

            # Description of the modules and packages in the distribution
            package_dir = {"pysqlite2": "pysqlite2"},
            packages = ["pysqlite2", "pysqlite2.test"],
            scripts=[],
            data_files = [],
            ext_modules = [ext],
            cmdclass = {"build_ext": build_ext},

            classifiers = [
            "Development Status :: 3 - Alpha",
            "Intended Audience :: Developers",
            "License :: OSI Approved :: zlib/libpng License",
            "Operating System :: MacOS :: MacOS X",
            "Operating System :: Microsoft :: Windows",
            "Operating System :: POSIX",
            "Programming Language :: C",
            "Programming Language :: Python",
            "Topic :: Database :: Database Engines/Servers",
            "Topic :: Software Development :: Libraries :: Python Modules"]
            )
    return setup_args

def main():
    setup(**get_setup_args())

if __name__ == "__main__":
    main()
