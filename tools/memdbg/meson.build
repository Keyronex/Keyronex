project('memdbg', 'c', 'cpp')

flex = find_program('flex', required: false)
bison = find_program('bison', required: false)

if not flex.found()
  error('flex not found.')
endif

if not bison.found()
  error('bison not found.')
endif

lgen = generator(flex,
output : '@PLAINNAME@.yy.cc',
arguments : ['-o', '@OUTPUT@', '@INPUT@'])

lfiles = lgen.process('lexer.l')

pgen = generator(bison,
output : ['@BASENAME@.tab.cc', '@BASENAME@.tab.h'],
arguments : ['@INPUT@', '--defines=@OUTPUT1@', '--output=@OUTPUT0@'])

pfiles = pgen.process('parser.y')

e = executable('pgen', 'main.cc',
    lfiles,
    pfiles)

