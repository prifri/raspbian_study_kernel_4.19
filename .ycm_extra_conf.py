# This file is NOT licensed under the GPLv3, which is the license for the rest
# of YouCompleteMe.
#
# Here's the license text for this file:
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# For more information, please refer to <http://unlicense.org/>

import os
import subprocess
import ycm_core

DIR_OF_THIS_SCRIPT = os.path.abspath(os.path.dirname(__file__))
DIR_OF_THIRD_PARTY = os.path.join(DIR_OF_THIS_SCRIPT, 'third_party')
SOURCE_EXTENSIONS = ['.cpp', '.cxx', '.cc', '.c', '.m', '.mm']

flags = ['-x', 'c', '-W', '-Wall', '-Werror',
         '-Wno-unused-function',
         '-Wno-error=unused-function',
         '-Wno-error=unused-parameter',
         '-Wno-address-of-packed-member',
         '-Wno-array-bounds',
         '-Wno-unused-parameter',

         '-I./include',
         '-I./arch/arm/include',
         ]

compilation_database_folder = ''

if os.path.exists(compilation_database_folder):
        database = ycm_core.CompilationDatabase(compilation_database_folder)
else:
        database = None


def IsHeaderFile(filename):
        extension = os.path.splitext(filename)[1]
        return extension in ['.h', '.hxx', '.hpp', '.hh']


def FindCorrespondingSourceFile(filename):
        if IsHeaderFile(filename):
                basename = os.path.splitext(filename)[0]
                for extension in SOURCE_EXTENSIONS:
                        replacement_file = basename + extension
                        if os.path.exists(replacement_file):
                                return replacement_file
        return filename


def Settings(**kwargs):
        if kwargs['language'] == 'cfamily':
                filename = FindCorrespondingSourceFile(kwargs['filename'])

                if not database:
                        return {
                                'flags': flags,
                                'include_paths_relative_to_dir':
                                DIR_OF_THIS_SCRIPT,
                                'override_filename': filename
                        }

                compilation_info = database.GetCompilationInfoForFile(filename)
                if not compilation_info.compiler_flags_:
                        return {}

                final_flags = list(compilation_info.compiler_flags_)

                try:
                        final_flags.remove('-stdlib=libc++')
                except ValueError:
                        pass

                return {
                        'flags': final_flags,
                        'include_paths_relative_to_dir':
                        compilation_info.compiler_working_dir_,
                        'override_filename': filename
                }
        return {}


def GetStandardLibraryIndexInSysPath(sys_path):
        for path in sys_path:
                if os.path.isfile(os.path.join(path, 'os.py')):
                        return sys_path.index(path)
        raise RuntimeError(
                'Could not find standard library path in Python path.')


def PythonSysPath(**kwargs):
        sys_path = kwargs['sys_path']
        for folder in os.listdir(DIR_OF_THIRD_PARTY):
                if folder == 'python-future':
                        folder = os.path.join(folder, 'src')
                        sys_path.insert(
                                        GetStandardLibraryIndexInSysPath
                                        (sys_path)+1,
                                        os.path.realpath(
                                                         os.path.join
                                                         (DIR_OF_THIRD_PARTY,
                                                          folder)))
                        continue

                if folder == 'cregex':
                        interpreter_path = kwargs['interpreter_path']
                        major_version = subprocess.check_output([
                                interpreter_path,
                                '-c', 'import sys; print(sys.version_info[0])']
                                ).rstrip().decode('utf8')
                        folder = os.path.join(
                                              folder,
                                              'regex_{}'.format(major_version))

                sys_path.insert(0, os.path.realpath(
                                                 os.path.join(
                                                    DIR_OF_THIRD_PARTY,
                                                    folder)))
        return sys_path
