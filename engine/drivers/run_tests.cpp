// Copyright 2011 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "engine/action.hpp"
#include "engine/context.hpp"
#include "engine/drivers/run_tests.hpp"
#include "engine/filters.hpp"
#include "engine/test_program.hpp"
#include "engine/test_result.hpp"
#include "engine/user_files/kyuafile.hpp"
#include "store/backend.hpp"
#include "store/transaction.hpp"
#include "utils/defs.hpp"
#include "utils/format/macros.hpp"
#include "utils/logging/macros.hpp"

namespace fs = utils::fs;
namespace run_tests = engine::drivers::run_tests;
namespace user_files = engine::user_files;


namespace {


/// Test case hooks to save the output into the database.
class file_saver_hooks : public engine::test_case_hooks {
    /// Open write transaction for the test case's data.
    store::transaction& _tx;

    /// Identifier of the test case being stored.
    const int64_t _test_case_id;

public:
    /// Constructs a new set of hooks.
    ///
    /// \param tx_ Open write transaction for the test case's data.
    /// \param test_case_id_ Identifier of the test case being stored.
    file_saver_hooks(store::transaction& tx_,
                     const int64_t test_case_id_) :
        _tx(tx_), _test_case_id(test_case_id_)
    {
    }

    /// Stores the stdout of the test case into the database.
    ///
    /// \param file Path to the stdout of the test case.
    void
    got_stdout(const fs::path& file)
    {
        _tx.put_test_case_file("__STDOUT__", file, _test_case_id);
    }

    /// Stores the stderr of the test case into the database.
    ///
    /// \param file Path to the stderr of the test case.
    void
    got_stderr(const fs::path& file)
    {
        _tx.put_test_case_file("__STDERR__", file, _test_case_id);
    }
};


/// Runs a test program in a controlled manner.
///
/// If the test program fails to provide a list of test cases, a fake test case
/// named '__test_program__' is created and it is reported as broken.
///
/// \param test_program The test program to execute.
/// \param config The configuration variables provided by the user.
/// \param filters The matching state of the filters.
/// \param hooks The user hooks to receive asynchronous notifications.
/// \param tx The store transaction into which to put the results.
/// \param action_id The action this program belongs to.
void
run_test_program(const engine::base_test_program& test_program,
                 const user_files::config& config,
                 engine::filters_state& filters,
                 run_tests::base_hooks& hooks,
                 store::transaction& tx,
                 const int64_t action_id)
{
    LI(F("Processing test program '%s'") % test_program.relative_path());
    const int64_t test_program_id = tx.put_test_program(test_program,
                                                        action_id);

    const engine::test_cases_vector& test_cases = test_program.test_cases();
    for (engine::test_cases_vector::const_iterator iter = test_cases.begin();
         iter != test_cases.end(); iter++) {
        const engine::test_case_ptr test_case = *iter;

        if (!filters.match_test_case(test_program.relative_path(),
                                     test_case->name()))
            continue;

        const int64_t test_case_id = tx.put_test_case(*test_case,
                                                      test_program_id);
        file_saver_hooks test_hooks(tx, test_case_id);
        const engine::test_result result = test_case->run(config, test_hooks);
        tx.put_result(result, test_case_id);
        hooks.got_result(test_case, result);
    }
}


}  // anonymous namespace


/// Pure abstract destructor.
run_tests::base_hooks::~base_hooks(void)
{
}


/// Executes the operation.
///
/// \param kyuafile_path The path to the Kyuafile to be loaded.
/// \param store_path The path to the store to be used.
/// \param raw_filters The test case filters as provided by the user.
/// \param config The end-user configuration properties.
/// \param hooks The hooks for this execution.
///
/// \returns A structure with all results computed by this driver.
run_tests::result
run_tests::drive(const fs::path& kyuafile_path,
                 const fs::path& store_path,
                 const std::set< engine::test_filter >& raw_filters,
                 const user_files::config& config,
                 base_hooks& hooks)
{
    const user_files::kyuafile kyuafile = user_files::kyuafile::load(
        kyuafile_path);
    filters_state filters(raw_filters);
    store::backend db = store::backend::open_rw(store_path);
    store::transaction tx = db.start();

    engine::context context = engine::context::current();
    const int64_t context_id = tx.put_context(context);

    engine::action action(context);
    const int64_t action_id = tx.put_action(action, context_id);

    for (test_programs_vector::const_iterator iter =
         kyuafile.test_programs().begin();
         iter != kyuafile.test_programs().end(); iter++) {
        const test_program_ptr& test_program = *iter;

        if (!filters.match_test_program(test_program->relative_path()))
            continue;

        run_test_program(*test_program, config, filters, hooks,
                         tx, action_id);
    }

    tx.commit();

    return result(action_id, filters.unused());
}
