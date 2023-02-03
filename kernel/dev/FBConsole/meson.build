gen = generator(bin2c,
    output : '@PLAINNAME@.c',
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@', '-s', '@BASENAME@'  ]
)

kern_srcs += files('FBConsole.m', 'gterm.c', 'image.c', 'term.c')
kern_srcs += gen.process(['sun12x22.psfu', 'nbsdbold.psfu'])
