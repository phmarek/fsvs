set exrc
" ignore FSVS messages in "make run-tests"
set efm^=%-Gcommitted\ revision%\\t%\\d%#\ on\ %.%#\ as\ %.%#
set efm^=%-GAn\ error\ occurred\ at\ %.%#
set tags+=tags
set tags+=src/tags

set errorformat+=\ \ "%f":1:\ (%s

set efm^=%-Gmake%.%#Makefile%.%#run%.%#tests%.%#Fehler%.%#

execute "source " . expand("<sfile>:h") . "/.vimrc.syntax"
