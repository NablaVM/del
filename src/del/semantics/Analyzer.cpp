#include "Analyzer.hpp"

#include <iostream>
#include <regex>
#include <sstream>

#include "SystemSettings.hpp"

#define N_UNUSED(x) (void)x;

namespace DEL
{
namespace
{
    bool is_only_number(std::string v)
    {
        try
        {
            double value = std::stod(v);
            N_UNUSED(value)
            return true;
        }
        catch(std::exception& e)
        {
            // Its not a number
        }
        return false;
    }
}
    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    Analyzer::Analyzer(Errors & err, SymbolTable & symbolTable, Codegen & code_gen, Memory & memory) : 
                                                                        error_man(err), 
                                                                        symbol_table(symbolTable),
                                                                        memory_man(memory),
                                                                        endecoder(memory_man),
                                                                        intermediate_layer(memory, code_gen)
    {
        program_watcher.setup();
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    Analyzer::~Analyzer()
    {
        
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::check_for_finalization()
    {
        if(!program_watcher.has_main)
        {
            error_man.report_no_main_function();
        }
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::ensure_unique_symbol(std::string id, int line_no)
    {
        if(symbol_table.does_symbol_exist(id, true))
        {
            error_man.report_previously_declared(id, line_no);
        }
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::ensure_id_in_current_context(std::string id, int line_no, std::vector<ValType> allowed)
    {
        // Check symbol table to see if an id exists, don't display information yet
        if(!symbol_table.does_symbol_exist(id, false))
        {
            // Reports the error and true marks the program for death
            error_man.report_unknown_id(id, line_no, true);
        }

        // If allowed is empty, we just wanted to make sure the thing existed
        if(allowed.size() == 0)
        {
            return;
        }

        // Ensure type is one of the allowed types
        bool is_allowed = false;
        for(auto & v : allowed)
        {
            if(symbol_table.is_existing_symbol_of_type(id, v))
            {
            is_allowed = true;
            }
        }

        // If the type isn't allowed
        if(!is_allowed)
        {
            error_man.report_unallowed_type(id, line_no, true);
        }
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    ValType Analyzer::get_id_type(std::string id, int line_no)
    {
        ValType t = symbol_table.get_value_type(id);

        if(t == ValType::NONE)
        {
            error_man.report_unknown_id(id, line_no, true);
        }

        return t;
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::build_function(Function *function)
    {
        // Ensure function is unique
        if(symbol_table.does_context_exist(function->name))
        {
            // Dies if not unique
            error_man.report_previously_declared(function->name, function->line_no);
        }

        symbol_table.new_context(function->name);

        // Check for 'main'
        if(function->name == "main")
        {
            program_watcher.has_main = true;
        }

        // Check for passign the hard-set limit on parameters
        if(function->params.size() > SETTINGS::GS_FUNC_PARAM_RESERVE)
        {
            std::string error = " Given function exceeds current limit of '" + std::to_string(SETTINGS::GS_FUNC_PARAM_RESERVE) + "' parameters";

            error_man.report_custom("Analyzer::build_function", error, true);
        }

        // Create function context
        // Don't remove previous context.. we clear the variables out later

        // Place function parameters into context
        for(auto & p : function->params)
        {
            symbol_table.add_symbol(p.id, p.type);
        }

        // Add parameters to the context
        symbol_table.add_parameters_to_current_context(function->params);

        // Add return type to the context
        symbol_table.add_return_type_to_current_context(function->return_type);

        // Tell intermediate layer to start function with given parametrs
        intermediate_layer.issue_start_function(function->name, function->params);

        // So elements can access function information as we visit them
        current_function = function;

        // Keep an eye out for pieces that we enforce in a function
        function_watcher.has_return = false;

        // Iterate over function elements and visit them with *this
        for(auto & el : function->elements)
        {
            // Visiting elements will trigger analyzer to check particular element
            // for any errors that may be present, and then analyzer will ask Intermediate to
            // generate instructions for the Codegen / Send the instructions to code gen
            el->visit(*this);

            // Now that the item is constructed, we free the memory
            delete el;
        }

        // Tell intermediate layer that we are done constructin the current function
        intermediate_layer.issue_end_function();

        // Clear the symbol table for the given function so elements cant be accessed externally
        // We dont delete the context though, that way can confirm existence later
        symbol_table.clear_existing_context(function->name);

        if(!function_watcher.has_return)
        {
            error_man.report_no_return(function->name, function->line_no);
        }

        current_function = nullptr;

        // Reset the memory manager for alloc variables in new space
        memory_man.reset();

        // Function is constructed - and elements have been freed
        delete function;
    }

    // -----------------------------------------------------------------------------------------
    // 
    //                              Visitor Methods
    //
    // -----------------------------------------------------------------------------------------

    void Analyzer::accept(Assignment &stmt)
    {
        /*
            If the assignment is a reassignment, it will be indicated via REQ_CHECK. 
            If this is active, we need to ensure it exists in reach and ensure that the type 
            allows us to do assignments. 
        */

        Memory::MemAlloc memory_info;

        bool requires_ds_allocation = true;
        bool requires_allocation_in_symbol_table = true;
        if(stmt.data_type == ValType::REQ_CHECK)
        {
            // Check that the rhs is in context and is a type that we are allowing for assignment
            ensure_id_in_current_context(stmt.lhs, stmt.line_no, {ValType::INTEGER, ValType::REAL, ValType::CHAR});

            // Now we know it exists, we set the data type to what it states in the map
            stmt.data_type = get_id_type(stmt.lhs, stmt.line_no);

            // We already checked symbol table if this exists, and symbol table is what 
            // handles allocation of memory for the target, so this is safe
            memory_info = memory_man.get_mem_info(stmt.lhs);

            requires_allocation_in_symbol_table = false;
            requires_ds_allocation = false;
        }
        else
        {
            // If this isn't a reassignment, we need to ensure that the value is unique
            ensure_unique_symbol(stmt.lhs, stmt.line_no);
        }

        /*
            Call the validation method to walk the assignment AST and build an instruction set for the 
            code generator while analyzing the data to ensure that all variables exist and to pull the type
            that the resulting operation would be
        */
        INTERMEDIATE::TYPES::AssignmentClassifier classification = INTERMEDIATE::TYPES::AssignmentClassifier::INTEGER; // Assume int 
        
        std::string postfix_expression = validate_assignment_ast(stmt.line_no, stmt.rhs, classification, stmt.data_type, stmt.lhs);

        // The unique value doesn't exist yet and needs some memory allocated and 
        // needs to be added to the symbol table
        if(requires_allocation_in_symbol_table)
        {
            // If this fails, we all fail. add_symbol will figure out data size 
            // and add to memory manager (Until we build more complicated structs
            // then we will have to update this call ) <<<<<<<<<<<<<<<<<<<<<<<<, TODO
            symbol_table.add_symbol(stmt.lhs, stmt.data_type);

            // No longer requires allocation
            requires_allocation_in_symbol_table = false;

            // Retrieve the generated memory information
            memory_info = memory_man.get_mem_info(stmt.lhs);
        }

        intermediate_layer.issue_assignment(stmt.lhs, requires_ds_allocation, memory_info, classification, postfix_expression);
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::accept(ReturnStmt & stmt)
    {
        // Create a 'variable assignment' for the return so we can copy the value or whatever
        std::string variable_for_return = symbol_table.generate_unique_return_symbol();

        function_watcher.has_return = true;

        // Handle NIL / NONE Return
        if(stmt.data_type == ValType::NONE)
        {
            intermediate_layer.issue_null_return();
            return;
        }

        // Create an assignment for the return, this will execute the return withing code gen as we set a RETURN node type that is processed by the assignment
        Assignment * return_assignment = new Assignment(current_function->return_type, variable_for_return, new DEL::AST(DEL::NodeType::RETURN, stmt.rhs, nullptr));
        return_assignment->line_no = stmt.line_no;
        this->accept(*return_assignment);

        delete return_assignment;
    }

    // ----------------------------------------------------------
    // This is a call statment on its own, not in an expression
    // ----------------------------------------------------------

    void Analyzer::accept(Call & stmt)
    {
        validate_call(stmt);

        ValType callee_type = symbol_table.get_return_type_of_context(stmt.name);

        if(callee_type != ValType::NONE)
        {
            error_man.report_calls_return_value_unhandled(current_function->name, stmt.name, stmt.line_no, false);
        }

        // We endocde it to leverage the same functionality that is required by an expression-based call
        intermediate_layer.issue_direct_call(
            endecoder.encode_call(&stmt)
        );
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::accept(If & stmt)
    {
        If * if_ptr = & stmt;

        std::string artificial_context = symbol_table.generate_unique_context();
        symbol_table.new_context(artificial_context, false );

        // Setup variables for conditions
        while(if_ptr != nullptr)
        {
            std::string if_condition_variable = symbol_table.generate_unique_variable_symbol();

            // Attempt to determine the type of the expression
            ValType condition_type = determine_expression_type(if_ptr->expr, if_ptr->expr, true, if_ptr->line_no);

            std::string value = (condition_type == ValType::REAL) ? "0.0" : "0";
            DEL::AST * artificial_value = new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, condition_type, value);
            DEL::AST * artificial_check = new DEL::AST(DEL::NodeType::GT , if_ptr->expr, artificial_value);

            // Create an assignment for the conditional 
            Assignment * c_assign = new Assignment(condition_type, if_condition_variable, artificial_check);

            c_assign->line_no = if_ptr->line_no;
            c_assign->visit(*this);

            delete artificial_value;
            delete artificial_check;
            delete c_assign;

            if_ptr->set_var_name(if_condition_variable);

            // Inc the if_ptr to its trail. Check for nullptr so we dont attempt to stati cast nothing
            if_ptr = (if_ptr->trail == nullptr) ? nullptr : static_cast<If*>(if_ptr->trail);
        }

        // Now that the conditionals are set, we can build the if statements
        build_if_stmt(stmt);

        // Remove the current context from the symbol table
        // This will remove all elements allocated by id from the memory manager as well
        // while preserving their id incs. 
        symbol_table.remove_current_context();
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::build_if_stmt(If & stmt)
    {
        switch(stmt.type)
        {
            case IfType::IF:
            {
                std::string artificial_context = symbol_table.generate_unique_context();

                // Create an artificial context in symbol table for the current if statement
                symbol_table.new_context(artificial_context, false );

                // Initiate the start of the conditional - memory should have been assigned to the variable by the assignment above
                intermediate_layer.issue_start_conditional_context(memory_man.get_mem_info(stmt.var_name));

                // Visit all statements in the conditional
                for(auto & el : stmt.element_list)
                {
                    el->visit(*this);
                    delete el;
                }

                // Remove the current context from the symbol table
                // This will remove all elements allocated by id from the memory manager as well
                // while preserving their id incs. 
                symbol_table.remove_current_context();

                // If theres a trailing elif or else
                if(stmt.trail != nullptr)
                {
                    If * ifp = static_cast<If*>(stmt.trail);
                    build_if_stmt(*ifp);
                }
                break;
            }
            case IfType::ELIF:
            case IfType::ELSE:
            {
                std::string artificial_context = symbol_table.generate_unique_context();

                // Create an artificial context in symbol table for the current if statement
                symbol_table.new_context(artificial_context, false );

                // Initiate the start of the conditional - memory should have been assigned to the variable by the assignment above
                intermediate_layer.issue_trailed_context(memory_man.get_mem_info(stmt.var_name));

                // Visit all statements in the conditional
                for(auto & el : stmt.element_list)
                {
                    el->visit(*this);
                    delete el;
                }

                // Remove the current context from the symbol table
                // This will remove all elements allocated by id from the memory manager as well
                // while preserving their id incs. 
                symbol_table.remove_current_context();

                if(stmt.trail != nullptr)
                {
                    If * ifp = static_cast<If*>(stmt.trail);
                    build_if_stmt(*ifp);
                }
                return;
            }
            default:
                std::cout << "If statement : DEFAULT : " << (int)stmt.type << std::endl;
                return;
        }

        // Mark that we are done with the if/elif/else chains. For this to work, we have to BREAK from base IF, and RETURN
        // from the trailing elif / else checks (I think)
        intermediate_layer.issue_end_conditional_context();
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::accept(ForLoop & stmt)
    {
        // Ensure that the range isn't bonked
        validate_range(stmt.range, stmt.type);

        // Validate step
        validate_step(stmt.line_no, stmt.step, stmt.type);

        // Create a context for the loop
        std::string artificial_context = symbol_table.generate_unique_context();
        symbol_table.new_context(artificial_context, false );

        // Create a name for the end variable
        std::string end_var;

        if(stmt.range->type == ValType::REQ_CHECK)
        {
            if(is_only_number(stmt.range->to))
            {
                end_var = symbol_table.generate_unique_variable_symbol();

                // Create the end variable, and assign it to the final position (to)
                Assignment * assign_end_var = new Assignment(stmt.type, end_var, new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, stmt.type, stmt.range->to));
                assign_end_var->line_no = stmt.line_no;
                this->accept(*assign_end_var);
                delete assign_end_var;
            }
            else
            {
                // Here we will use the raw var
                end_var = stmt.range->to;
            }

            if(is_only_number(stmt.range->from))
            {
                // Create the loop variable, and assign it to the initial position (from) represented by a raw value
                Assignment * assign_loop_var = new Assignment(stmt.type, stmt.id, new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, stmt.type, stmt.range->from));
                assign_loop_var->line_no = stmt.line_no;
                this->accept(*assign_loop_var);
                delete assign_loop_var;
            }
            else
            {
                // Create the loop variable, and assign it to the initial position (from) represented by a variable value
                Assignment * assign_loop_var = new Assignment(stmt.type, stmt.id, new DEL::AST(DEL::NodeType::ID, nullptr, nullptr, stmt.type, stmt.range->from));
                assign_loop_var->line_no = stmt.line_no;
                this->accept(*assign_loop_var);
                delete assign_loop_var;
            }
        }
        else
        {
            end_var = symbol_table.generate_unique_variable_symbol();

            // Create the end variable, and assign it to the final position (to)
            Assignment * assign_end_var = new Assignment(stmt.type, end_var, new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, stmt.type, stmt.range->to));
            assign_end_var->line_no = stmt.line_no;
            this->accept(*assign_end_var);
            delete assign_end_var;

            // Create the loop variable, and assign it to the initial position (from) represented by a raw value
            Assignment * assign_loop_var = new Assignment(stmt.type, stmt.id, new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, stmt.type, stmt.range->from));
            assign_loop_var->line_no = stmt.line_no;
            this->accept(*assign_loop_var);
            delete assign_loop_var;
        }

        // Setup 'step'
        std::string step_var_name;

        // If step is just a raw value, create a variable for it
        if(stmt.step->type != ValType::REQ_CHECK)
        {
            step_var_name = symbol_table.generate_unique_variable_symbol();

            // Create the step variable, and assign it to the final position (to)
            Assignment * assign_end_var = new Assignment(stmt.type, step_var_name, new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, stmt.type, stmt.step->val));
            assign_end_var->line_no = stmt.line_no;
            this->accept(*assign_end_var);
            delete assign_end_var;
        }

        // Otherwise, use the value given to us
        else
        {
            step_var_name = stmt.step->val;
        }

        // Translate data type
        INTERMEDIATE::TYPES::AssignmentClassifier classifier = (stmt.type == ValType::REAL) ? 
                INTERMEDIATE::TYPES::AssignmentClassifier::DOUBLE : 
                INTERMEDIATE::TYPES::AssignmentClassifier::INTEGER;

        // Create intermediate representation for the loop
        INTERMEDIATE::TYPES::ForLoop * ifl = new INTERMEDIATE::TYPES::ForLoop(classifier,
                                                                              memory_man.get_mem_info(stmt.id),
                                                                              memory_man.get_mem_info(end_var),
                                                                              memory_man.get_mem_info(step_var_name));
        // Start off the for loop
        intermediate_layer.issue_start_loop(ifl);
 
        // Compile the statements in the for loop
        for(auto & el : stmt.elements)
        {
            el->visit(*this);
            delete el;
        }

        // End the loop
        intermediate_layer.issue_end_loop(ifl);

        // Remove the context for the loop
        symbol_table.remove_current_context();

        delete ifl;
        delete stmt.step;
        delete stmt.range;
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::accept(WhileLoop & stmt)
    {
        // Determine the type of the expression
        ValType condition_type = determine_expression_type(stmt.expr, stmt.expr, true, stmt.line_no);

        // Create a context for the loop
        std::string artificial_context = symbol_table.generate_unique_context();
        symbol_table.new_context(artificial_context, false );

        std::string while_condition_variable = symbol_table.generate_unique_variable_symbol();

        // A reassignment statement to update artificial variable that checks the condition of the while
        // when loop is executed
        Assignment * update_condition;

        // Create a variable to mark the expression as true or false
        std::string value = (condition_type == ValType::REAL) ? "0.0" : "0";
        DEL::AST * artificial_value = new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, condition_type, value);
        DEL::AST * artificial_check = new DEL::AST(DEL::NodeType::GT , stmt.expr, artificial_value);

        // Create an assignment for the conditional 
        Assignment * c_assign = new Assignment(condition_type, while_condition_variable, artificial_check);

        // Create the conditional assignment

        c_assign->line_no = stmt.line_no;
        c_assign->visit(*this);

        // To update the while condition inside the loop
        update_condition = new DEL::Assignment(DEL::ValType::REQ_CHECK, while_condition_variable, artificial_check);
        update_condition->line_no = stmt.line_no;

        // Get the memory information
        Memory::MemAlloc condition_mem_alloc = memory_man.get_mem_info(while_condition_variable);

        // Translate data type
        INTERMEDIATE::TYPES::AssignmentClassifier classifier = (condition_type == ValType::REAL) ? 
                INTERMEDIATE::TYPES::AssignmentClassifier::DOUBLE : 
                INTERMEDIATE::TYPES::AssignmentClassifier::INTEGER;

        // Indicate loop start to intermediate
        INTERMEDIATE::TYPES::WhileLoop * while_loop = new INTERMEDIATE::TYPES::WhileLoop(classifier, condition_mem_alloc);

        // Start the loop
        intermediate_layer.issue_start_loop(while_loop);

        // Now that we've started the loop, we need to ensure the condition is updated each iteration
        update_condition->visit(*this);

        // Cleanup of things we don't need anymore
        delete artificial_value;
        delete artificial_check;
        delete c_assign;
        delete update_condition;


        // Compile the statements in the for loop
        for(auto & el : stmt.elements)
        {
            el->visit(*this);
            delete el;
        }

        // End the loop
        intermediate_layer.issue_end_loop(while_loop);

        // Remove the context for the loop
        symbol_table.remove_current_context();

        delete while_loop;
    }

    // ----------------------------------------------------------
    // Named loops are just while loops under the hood
    // ----------------------------------------------------------

    void Analyzer::accept(NamedLoop & stmt)
    {
        std::string artificial_context = symbol_table.generate_unique_context();
        symbol_table.new_context(artificial_context, false );

        // Ensure the symbol for the loop name is unique
        ensure_unique_symbol(stmt.name, stmt.line_no);

        // Create a variable for the named loop
        DEL::AST * loop_variable = new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, ValType::INTEGER, "1");
        Assignment * c_assign = new Assignment(ValType::INTEGER, stmt.name, loop_variable); 
        c_assign->line_no = stmt.line_no;
        c_assign->visit(*this);

        delete loop_variable;
        delete c_assign;

        // Make an expression that is the loop name 
        DEL::AST * expr = new DEL::AST(DEL::NodeType::ID,  nullptr, nullptr, DEL::ValType::STRING,  stmt.name);

        // Create a while loop with that expression and the loop's elements  ==>  while(loop_name){ loop.eleemnts; }
        //
        DEL::WhileLoop * wl = new DEL::WhileLoop(expr, stmt.elements);

        wl->visit(*this);

        // Wl will be deleted by the function that it accepts to

        // Remove the context for the loop
        symbol_table.remove_current_context();
    }

    // ----------------------------------------------------------
    // Annulments set an int or double to their representation of 0
    // ----------------------------------------------------------

    void Analyzer::accept(AnnulStmt & stmt)
    {
        // Ensure variable exists
        ensure_id_in_current_context(stmt.var, stmt.line_no, {ValType::INTEGER, ValType::REAL});

        DEL::AST * annul_val;

        // Create the correct annulment
        if(symbol_table.is_existing_symbol_of_type(stmt.var, ValType::REAL))
        {
            annul_val = new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, ValType::REAL, "0.0");
        }
        else
        {
            annul_val = new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, ValType::INTEGER, "0");
        }

        // Assignment to annul the variable
        DEL::Assignment * annulment = new DEL::Assignment(DEL::ValType::REQ_CHECK, stmt.var, annul_val); 
        annulment->set_line_no(stmt.line_no);

        // Execute assignment
        annulment->visit(*this);

        // Cleanup
        delete annul_val;
        delete annulment;
    }

    // -----------------------------------------------------------------------------------------
    // 
    //                              Validation Methods
    //
    // -----------------------------------------------------------------------------------------

    void Analyzer::validate_step(int line, Step * step, ValType loop_type)
    {
        if(step->type == ValType::REQ_CHECK)
        {
            // If the step is a variable all we can do is ensure that the step variable
            // exists and matches the type of the loop
            ensure_id_in_current_context(step->val, line, {loop_type});
            return;
        }

        // Ensure that the step type matches the loop type
        if(step->type != loop_type)
        {
            error_man.report_unallowed_type("step", line, true);
        }

        if(ValType::INTEGER == step->type)
        {
            uint64_t s = std::stoull(step->val);

            if(s == 0)
            {
                error_man.report_invalid_step(line);
            }
        }
        else if(ValType::REAL == step->type)
        {
            double s = std::stod(step->val);

            if(s == 0.0)
            {
                error_man.report_invalid_step(line);
            }
        }
        else
        {
            error_man.report_custom("Analyzer", 
                " Developer Error: A step to be validated came in with an unhandled type, grammar should've stopped this",
                true);
        }
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::validate_range(Range * range, ValType loop_type)
    {
        // Integers
        if(ValType::INTEGER == range->type)
        {
            uint64_t start = std::stoull(range->from);
            uint64_t end   = std::stoull(range->to);

            if(start > end)
            {
                error_man.report_range_invalid_start_gt_end(range->line_no, range->from, range->to);
            }

            if(start == end)
            {
                error_man.report_range_ineffective(range->line_no, range->from, range->to);
            }
            return;
        }

        // Reals
        if(ValType::REAL == range->type)
        {
            double start = std::stod(range->from);
            double end   = std::stod(range->to);

            if(start > end)
            {
                error_man.report_range_invalid_start_gt_end(range->line_no, range->from, range->to);
            }

            if(start == end)
            {
                error_man.report_range_ineffective(range->line_no, range->from, range->to);
            }
            return;
        }

        // Something.. more complicated
        if (ValType::REQ_CHECK == range->type)
        {
            // Check from and to, if they aren't a number then they are a variable.
            // If they are a variable we need to ensure the variable exists and is the same type as the range statement
            if(!is_only_number(range->from))
            {
                ensure_id_in_current_context(range->from, range->line_no, {loop_type});
            }

            if(!is_only_number(range->to))
            {
                ensure_id_in_current_context(range->to, range->line_no, {loop_type});
            }
            return;
        }


        // If we come here, an unhandled type cropped up
        error_man.report_custom("Analyser", 
            " Developer Error: A range to be validated came in with an unhandled type, grammar should've stopped this",
            true);
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::validate_call(Call & stmt)
    {
        // Disallow recursion until it is handled
        if(stmt.name == current_function->name)
        {
            error_man.report_custom("Analyzer", "Function recursion has not yet been implemented in Del", true);
        }

        // Ensure that the called method exists
        if(!symbol_table.does_context_exist(stmt.name))
        {
            error_man.report_callee_doesnt_exist(stmt.name, stmt.line_no);
        }

        // Get the callee params
        std::vector<FunctionParam> callee_params = symbol_table.get_context_parameters(stmt.name);

        // Ensure number of params match
        if(stmt.params.size() != callee_params.size())
        {
            error_man.report_mismatched_param_length(current_function->name, stmt.name, callee_params.size(), stmt.params.size(), stmt.line_no);
        }

        // Ensure all paramters exist
        for(auto & p : stmt.params)
        {
            // If we need to get the type, get the type now that we know it exists
            if(p.type == ValType::REQ_CHECK)
            {
                // Ensure the thing exists, because REQ_CHECK dictates that the parameter is a variable, not a raw
                if(!symbol_table.does_symbol_exist(p.id))
                {
                    std::cerr << "Paramter in call to \"" << stmt.name << "\" does not exist in the current context" << std::endl;
                    error_man.report_unknown_id(p.id, stmt.line_no, true);
                }

                // Set the type to the type of the known variable
                p.type = get_id_type(p.id, stmt.line_no);
            }

            // If we didn't need to get the type, then it came in raw and we need to make a variable for it
            else
            {
                // Generate a unique label for the raw parameter
                std::string param_label = symbol_table.generate_unique_call_param_symbol();

                // Create an assignment for the variable
                Assignment * raw_parameter_assignment = new Assignment(p.type, param_label, 
                    new DEL::AST(DEL::NodeType::VAL, nullptr, nullptr, p.type, p.id)
                );
                raw_parameter_assignment->line_no = stmt.line_no;

                this->accept(*raw_parameter_assignment);

                delete raw_parameter_assignment;

                if(!symbol_table.does_symbol_exist(param_label))
                {
                    std::cerr << "Auto generated parameter variable in call to \"" << stmt.name << "\" did not exist after assignment" << std::endl;
                    error_man.report_unknown_id(param_label, stmt.line_no, true); 
                }

                // If the call works, which it should, then we update the id to the name of the varaible
                // so we can reference it later
                p.id = param_label;
            }
        }

        // Check that the param types match
        for(uint16_t i = 0; i < stmt.params.size(); i++ )
        {
            if(stmt.params[i].type != callee_params[i].type)
            {
                std::cerr << "Parameter \"" << stmt.params[i].id << "\" does not match expected type in paramter list of function \"" << stmt.name << "\"" << std::endl;
                error_man.report_unallowed_type(stmt.params[i].id, stmt.line_no, true);
            }
        }
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    ValType Analyzer::determine_expression_type(AST * ast, AST * traverse, bool left, int line_no)
    {
        if(ast->node_type == NodeType::VAL)
        {
            return ast->val_type;
        }
        else if (ast->node_type == NodeType::ID)
        {
            return get_id_type(ast->value, line_no);
        }
        else if (ast->node_type == NodeType::CALL)
        {
            Call * call = static_cast<Call*>(ast);
            return call->val_type;
        }

        if(left)
        {
            // This should never happen, but we handle it just in case
            if(ast->l == nullptr)
            {
                return determine_expression_type(traverse, traverse, false, line_no);
            }

            // Go down left side - we only need to traverse one side
            return determine_expression_type(ast->l, traverse, true, line_no);
        }
        else
        {
            // This REALLY should't happen.
            if(ast->r == nullptr)
            {
                error_man.report_custom("Analyzer::determine_expression_type()", " Developer error : Failed to determine expression type", true);
            }

            return determine_expression_type(ast->r, traverse, false, line_no);
        }
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::check_value_is_valid_for_assignment(int line_no, ValType type_to_check, INTERMEDIATE::TYPES::AssignmentClassifier & c, ValType & et, std::string & id)
    {
        switch(type_to_check)
        {
            case ValType::STRING   :    error_man.report_custom("Analyzer", " STRING found in arithmetic exp",    true); // Grammar should have
            case ValType::REQ_CHECK:    error_man.report_custom("Analyzer", " REQ_CHECK found in arithmetic exp", true); // filteres these out
            case ValType::NONE     :    error_man.report_custom("Analyzer", " NONE found in arithmetic exp",      true);
            case ValType::FUNCTION :    error_man.report_custom("Analyzer", " FUNCTION found in arithmetic exp",  true);
            case ValType::REAL     : 
            {
                // Promote to Double if any double is present
                c = INTERMEDIATE::TYPES::AssignmentClassifier::DOUBLE;
                if(et != ValType::REAL)
                {
                    std::string error_message = id;

                    // There are better ways to do this, but if it happens at all it will only happen once during the compiler run
                    // as we are about to die
                    if(std::regex_match(id, std::regex("(__return__assignment__).*")))
                    {
                        error_message = "Function (" + current_function->name + ")";
                    } 
                    error_man.report_unallowed_type(error_message, line_no, true); 
                }

                break;
            }
            case ValType::INTEGER  :
            {
                // We assume its an integer to start with so we dont set type (because we allow ints inside double exprs)
                if(et != ValType::INTEGER) { error_man.report_unallowed_type(id, line_no, true); }
                break;
            }
            case ValType::CHAR     :
            {
                c = INTERMEDIATE::TYPES::AssignmentClassifier::CHAR;
                if(et != ValType::CHAR)   { error_man.report_unallowed_type(id, line_no, true); } // If Assignee isn't a char, we need to die
                break;
            }
        }
    }

    // ----------------------------------------------------------
    // Assignee's expected type abbreviated to 'et' 
    // ----------------------------------------------------------

    std::string Analyzer::validate_assignment_ast(int line_no, AST * ast, INTERMEDIATE::TYPES::AssignmentClassifier & c, ValType & et, std::string & id)
    {
        switch(ast->node_type)
        {
            case NodeType::ID  : 
            {
                // Ensure the ID is within current context. Allowing any type
                ensure_id_in_current_context(ast->value, line_no, {});

                // Check for promotion
                ValType id_type = get_id_type(ast->value, line_no);

                // Make sure that the known value of the identifier is one valid given the current assignemnt
                check_value_is_valid_for_assignment(line_no, id_type, c, et, id);

                // Encode the identifier information so we can handle it in the intermediate layer
                return endecoder.encode_identifier(ast->value);
            }
            
            case NodeType::CALL :
            {
                // We know its a call, so lets treat it like a call
                Call * call = static_cast<Call*>(ast);

                // This call to validate_call will ensure that all parameters within the call exist in the system as variables
                // and it will update the current object to the new information we need to pull addresses
                validate_call(*call);

                // Ensure that the return type of the call is valid for the assignment
                // Our call to validate_call made sure that call->name exists as a context within symbol table
                // so we can use that value directly
                check_value_is_valid_for_assignment(
                    line_no,
                    symbol_table.get_return_type_of_context(call->name)
                    , c, et, id
                );
                
                // Encode the call to something we can handle in the intermediate layer
                return endecoder.encode_call(call);
            }

            case NodeType::VAL : 
            { 
                // Check that the raw value is one that is valid within the current assignment
                check_value_is_valid_for_assignment(line_no, ast->val_type, c, et, id);
                return ast->value;
            }
            
            //  This is where we convert NodeType to the Assignment type. This should make it so we can change the actual tokens in the language
            //  without having to modify this statement

            case NodeType::ADD    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " ADD    " );  
            case NodeType::SUB    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " SUB    " );
            case NodeType::DIV    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " DIV    " );
            case NodeType::MUL    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " MUL    " );
            case NodeType::MOD    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " MOD    " );
            case NodeType::POW    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " POW    " );
            case NodeType::LTE    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " LTE    " );
            case NodeType::GTE    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " GTE    " );
            case NodeType::GT     :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " GT     " );
            case NodeType::LT     :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " LT     " );
            case NodeType::EQ     :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " EQ     " );
            case NodeType::NE     :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " NE     " );
            case NodeType::LSH    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " LSH    " );
            case NodeType::RSH    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " RSH    " );
            case NodeType::BW_OR  :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " BW_OR  " );
            case NodeType::BW_XOR :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " BW_XOR " );
            case NodeType::BW_AND :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " BW_AND " );
            case NodeType::OR     :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " OR     " );
            case NodeType::AND    :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " " + validate_assignment_ast(line_no, ast->r, c, et, id) + " AND    " );
            case NodeType::BW_NOT :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " BW_NOT ");
            case NodeType::NEGATE :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " NEGATE "  );
            case NodeType::RETURN :return (validate_assignment_ast(line_no, ast->l, c, et, id) + " RETURN "  );
            case NodeType::ROOT   : error_man.report_custom("Analyzer", "ROOT NODE found in arithmetic exp", true); break;
            default:
            return "Its dead, jim";
        }
        return "Complete";
    }
}
