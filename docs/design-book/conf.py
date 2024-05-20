# Configuration file for the Sphinx documentation builder.

# -- Project information

project = 'Keyronex Operating System Design and Implementation'
copyright = '2020-2024, NetaScale Object Solutions Ltd.'
author = 'Keyronex-lite Authors'

release = '1.0'
version = '0.1.0'

# -- General configuration

extensions = [
    'sphinx.ext.duration',
    'sphinx.ext.doctest',
    'sphinx.ext.autodoc',
    'sphinx.ext.autosummary',
    'sphinx.ext.intersphinx',
	'sphinx.ext.todo',
]

intersphinx_mapping = {
    'python': ('https://docs.python.org/3/', None),
    'sphinx': ('https://www.sphinx-doc.org/en/master/', None),
}
intersphinx_disabled_domains = ['std']

templates_path = ['_templates']

master_doc = 'index'

todo_include_todos = 'true'

# -- Options for HTML output
html_theme = 'sphinx_rtd_theme'
html_logo = '../keyronex.svg'

# -- Options for EPUB output
epub_show_urls = 'footnote'
