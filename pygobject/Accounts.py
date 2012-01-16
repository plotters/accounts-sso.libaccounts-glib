from ..overrides import override
from ..importer import modules
from gi.repository import GObject

Accounts = modules['Accounts']._introspection_module

__all__ = []

def _get_string(self, key, default_value=None):
    value = GObject.Value()
    value.init(GObject.TYPE_STRING)
    if self.get_value(key, value) != Accounts.SettingSource.NONE:
        return value.get_string()
    else:
        return default_value

def _get_int(self, key, default_value=None):
    value = GObject.Value()
    value.init(GObject.TYPE_INT64)
    if self.get_value(key, value) != Accounts.SettingSource.NONE:
        return value.get_int64()
    else:
        return default_value

def _get_bool(self, key, default_value=None):
    value = GObject.Value()
    value.init(GObject.TYPE_BOOLEAN)
    if self.get_value(key, value) != Accounts.SettingSource.NONE:
        return value.get_boolean()
    else:
        return default_value

class Account(Accounts.Account):
    get_string = _get_string
    get_int = _get_int
    get_bool = _get_bool

Account = override(Account)
__all__.append('Account')

class AccountService(Accounts.AccountService):
    get_string = _get_string
    get_int = _get_int
    get_bool = _get_bool

AccountService = override(AccountService)
__all__.append('AccountService')

