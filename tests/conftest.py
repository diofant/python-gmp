from datetime import timedelta

from hypothesis import settings


default = settings.get_profile("default")
settings.register_profile("default",
                          settings(default,
                                   deadline=timedelta(seconds=300)))
ci = settings.get_profile("ci")
settings.register_profile("ci", settings(ci, max_examples=10000))
