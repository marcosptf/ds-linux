## Process this file with automake to produce Makefile.in -*-Makefile-*-.
AUTOMAKE_OPTIONS = 1.4 gnits

TESTS = fail-perm ignore t-slash

EXTRA_DIST = $(TESTS)
TESTS_ENVIRONMENT = \
  top_srcdir=$(top_srcdir) \
  srcdir=$(srcdir) \
  PERL="$(PERL)" \
  PATH="$(VG_PATH_PREFIX)`pwd`/../../src$(PATH_SEPARATOR)$$PATH" \
  PROG=rmdir
