# Configuration file for the Sphinx documentation builder.

# -- Project information

project = 'Design of the Keyronex Operating System'
copyright = '2020-2022, NetaScale Object Solutions Ltd.'
author = 'NetBSD User'

release = '1.0'
version = '0.1.0'

# -- General configuration

extensions = [
    'sphinx.ext.duration',
    'sphinx.ext.doctest',
    'sphinx.ext.autodoc',
    'sphinx.ext.autosummary',
    'sphinx.ext.intersphinx',
]

intersphinx_mapping = {
    'python': ('https://docs.python.org/3/', None),
    'sphinx': ('https://www.sphinx-doc.org/en/master/', None),
}
intersphinx_disabled_domains = ['std']

templates_path = ['_templates']

master_doc = 'index'


# -- Options for HTML output
html_theme = 'sphinx_rtd_theme'
html_logo = '../keyronexnofont_short.svg'

# -- Options for EPUB output
epub_show_urls = 'footnote'
