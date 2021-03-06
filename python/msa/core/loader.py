import importlib


# builtin modules to load
builtin_module_names = [
    "time",
    "tty",
    "command_registry",
    "echo",
    "command",
]


def load_builtin_modules():
    """Loads builtin modules."""
    plugin_modules = []


    for module_name in builtin_module_names:
        module = importlib.import_module("msa.builtins.{}".format(module_name))
        module.module_name = module_name
        plugin_modules.append(module)

    return plugin_modules


def load_plugin_modules(plugin_module_names, mode):
    """Loads plugin modules as specified in the configuration file.

    Parameters
    ----------
    plugin_module_names : List[str]
        Plugin module names to load. Module names should be fully qualified modules existing in `msa.plugins`.
    mode : `msa.core.RunMode`
        The mode the system is being run in.

    Returns
    -------

    """

    plugin_modules = []

    for module_name in plugin_module_names:
        module = importlib.import_module("msa.plugins.{}".format(module_name))
        module.module_name = module_name

        if mode in module.PluginModule.allowed_modes:
            plugin_modules.append(module)

    return plugin_modules
