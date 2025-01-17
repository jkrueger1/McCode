''' A collection of flask-related utility functions '''

from functools import wraps

from app import app
from flask import request, make_response, render_template, redirect, url_for

from models import User

from werkzeug.security import check_password_hash
from sqlalchemy.orm.exc import NoResultFound

from util import new_key


def one_or_none(query):
    ''' Try to extract one row from query, return None when not possible '''
    try:
        return query.one()
    except NoResultFound:
        return None


def skip(disallowed, items):
    ''' Skip disallowed elements '''
    for i in items:
        if i not in disallowed:
            yield i


def templated(template=None):
    ''' Attach a template to a response handler '''
    def decorator(handle):
        ''' Decorator around handler '''
        @wraps(handle)
        def decorated_function(*args, **kwargs):
            ''' Gets called in place of handler '''
            template_name = template
            if template_name is None:
                template_name = request.endpoint \
                    .replace('.', '/') + '.html'
            ctx = handle(*args, **kwargs)
            if ctx is None:
                ctx = {}
            elif not isinstance(ctx, dict):
                return ctx
            return render_template(template_name, **ctx)
        return decorated_function
    return decorator


def get_session():
    ''' Return current session '''
    return app.open_session(request)


def get_nonce():
    ''' Get the nonce registered with session '''
    return get_session().get('nonce', None)


def with_nonce():
    ''' Add a random nonce for forms etc. in a secure cookie '''
    def decorator(handle):
        ''' Decorator around handler '''
        @wraps(handle)
        def decorated_function(*args, **kwargs):
            ''' Gets called in place of handler '''
            session = get_session()
            if not 'nonce' in session:
                session['nonce'] = new_key()
            resp = make_response(handle(*args, **kwargs))
            app.save_session(session, resp)
            return resp
        return decorated_function
    return decorator


def check_nonce():
    ''' Make sure nonce from session matches form '''
    def decorator(handle):
        ''' Decorator around handler '''
        @wraps(handle)
        def decorated_function(*args, **kwargs):
            ''' Gets called in place of handler '''
            session = get_session()
            if 'nonce' not in session:
                return 'Denied', 403
            if session['nonce'] != request.form.get('__nonce', None):
                return 'Denied', 403
            return handle(*args, **kwargs)
        return decorated_function
    return decorator


def authenticated(allowed=None, redirect_login=True):
    ''' Make sure user is authenticated '''
    def decorator(handle):
        ''' Decorator around handler '''
        @wraps(handle)
        def decorated_function(*args, **kwargs):
            ''' Gets called in place of handle '''
            session = get_session()
            user = session.get('user', None)
            if user is None or (allowed and user not in allowed):
                if redirect_login:
                    return redirect(url_for('login', next=request.path.lstrip('/')))
                return 'Denied', 403
            return handle(*args, user=user, **kwargs)
        return decorated_function
    return decorator


def authenticate(username, password):
    ''' Check whether a user can be authenticated '''
    if '' in (username, password):
        return False
    # check information
    users = User.query.filter_by(username=username).all()
    if not users or not check_password_hash(users[0].passhash, password):
        return False
    # All ok
    return True
