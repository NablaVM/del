#include "Analysis/Analyzer.hpp"

#include "del_driver.hpp"

// Forge
#include "SystemSettings.hpp"

#include "forge/constructs/Variable.hpp"
#include "forge/datatypes/DataType.hpp"

#include "forge/instructions/Instruction.hpp"
#include "forge/instructions/Assignment.hpp"
#include "forge/instructions/Call.hpp"
#include "forge/instructions/If.hpp"
#include "forge/instructions/Reassignment.hpp"
#include "forge/instructions/Return.hpp"
#include "forge/instructions/While.hpp"
#include "forge/instructions/For.hpp"
#include "forge/instructions/Continuable.hpp"


#include <iostream>

namespace DEL
{
    //
    // ===============---------------------------- Analyzer Methods ---------------------------===============
    //
    
    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    Analyzer::Analyzer(DEL_Driver & driver) : driver(driver)
    {
        program_watcher.has_main    = false;
        function_watcher.has_return = false;
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    Analyzer::~Analyzer()
    {

    }

    void Analyzer::report_incomplete(std::string what)
    {
        driver.code_forge.get_reporter().issue_report(
            new FORGE::InternalReport(
                {
                    "DEL::Analyzer",
                    "Analyzer.cpp",
                    "report_incomplete",
                    {
                        "The following has been detected by analyzer but is not yet complete:",
                        ("\t" + what) 
                    }
                }
            )
        );
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(Function & stmt) 
    {
        // Ensure function name doesn't exist

        if(driver.symbol_table.does_context_exist(stmt.name))
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(stmt.line_number), 
                    27, 
                    "Duplicate context name (" + stmt.name + ") detected", 
                    {"Rename function to be unique"}
                )
            );
        }

        driver.symbol_table.new_context(stmt.name);

        if(stmt.name == "main")
        {
            program_watcher.has_main = true;
        }

        // Ensure parameters aren't too many in number
        if(stmt.params.size() > FORGE::SETTINGS::GS_FUNC_PARAM_RESERVE)
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(stmt.line_number),
                    -1, 
                    driver.preprocessor.fetch_line(stmt.line_number),
                    {
                        "Function parameters exceed number permitted by system (" + std::to_string(FORGE::SETTINGS::GS_FUNC_PARAM_RESERVE) + ")",
                        "Reduce the number of parameters for the given function"
                    }
                )
            );
        }

        // Make a copy of the parameters
        std::vector<FORGE::Variable> params;
        for(auto & p : stmt.params)
        {
            params.push_back(*p);
        }

        // Add the parameters
        driver.symbol_table.add_parameters_to_current_context(params);

        // Add the return type
        driver.symbol_table.add_return_type_to_current_context(stmt.return_type->dataType);

        function_watcher.has_return = false;

        FORGE::Function * current_forge_function = new FORGE::Function(stmt.name , stmt.return_type->dataType);

        aggregators.push(current_forge_function);

        current_forge_aggregator = aggregators.top();

        current_front_function = &stmt;

        for(auto & el : stmt.elements)
        {
            el->visit(*this);
            delete el;
        }

        // Clear the symbol table for the given function so elements cant be accessed externally
        // We dont delete the context though, that way can confirm existence later
        driver.symbol_table.clear_existing_context(stmt.name);

        // Check that the function has been explicitly returned at the end of the function
        if(!function_watcher.has_return)
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(stmt.line_number),
                    -1, 
                    driver.preprocessor.fetch_line(stmt.line_number),
                    {
                        "Given function does not have a matching return. All functions must be explicitly returned"
                    }
                )
            );
        }

        // Add function to forge for later generation
        driver.code_forge.add_ready_function(current_forge_function);

        // Reset pointers
        current_forge_aggregator = nullptr;
        current_front_function = nullptr;
        aggregators.pop();

        // Reset memory manager for alloc variables in new functions
        driver.code_forge.reset_memory();

        // Clean params
        //for(auto & p : stmt.params)
        //{
        //    delete p;
        //}
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(Call & stmt) 
    {
        // Validate the call, and change any UNKNOWN types presented by variables being passed to their data type
        validate_call(stmt);

        // Create call and put in aggregator
        current_forge_aggregator->add_instruction(
            new FORGE::Call(stmt.params)
        );
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(Return & stmt) 
    {
        // If we are in the function context then we can say we have an explicit return
        std::string context_name = driver.symbol_table.get_current_context_name();

        if(context_name == current_front_function->name)
        {
            function_watcher.has_return = true;
        }
    
        // Check if the return has an expression associated with it
        bool has_return = (stmt.ast != nullptr);

        // If there is something to return, we need to construct the return
        if(has_return)
        {
            forge_expression_items.clear();

            // Build the expression items vector for the return, expect the type to be that declared by the function
            validate_and_build_assignment("Return Expression", stmt.ast, current_front_function->return_type->dataType, stmt.line_number);

            // Create the return and give to aggregator
            current_forge_aggregator->add_instruction(
                new FORGE::Return(
                    new FORGE::Expression(current_front_function->return_type->dataType, forge_expression_items)
                    )
                );
        }
        else
        {
            // Create the return and give to aggregator
            current_forge_aggregator->add_instruction(new FORGE::Return(nullptr));
        }
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(Assignment & stmt) 
    {
        if(driver.symbol_table.does_symbol_exist(stmt.ast->left->node.data))
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(stmt.line_number),
                    -1, 
                    driver.preprocessor.fetch_line(stmt.line_number),
                    {
                        "Symbol \"" + stmt.ast->left->node.data  + "\" used in assignment is not unique"
                    }
                )
            );
        }

        forge_expression_items.clear();

        // Build the expression items vector
        validate_and_build_assignment(stmt.ast->left->node.data, stmt.ast->right, stmt.type_info->dataType, stmt.line_number);

        FORGE::Assignment * assignment = new FORGE::Assignment(
            new FORGE::Variable(stmt.ast->left->node.data, stmt.type_info->dataType),
            new FORGE::Expression(stmt.type_info->dataType, forge_expression_items)
        );

        current_forge_aggregator->add_instruction(assignment);

        driver.symbol_table.add_symbol(stmt.ast->left->node.data, stmt.type_info->dataType, stmt.is_immutable);
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(Reassignment & stmt) 
    {
        // Ensure the symbol to be reassigned has already been defined
        if(!driver.symbol_table.does_symbol_exist(stmt.ast->left->node.data))
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(stmt.line_number),
                    -1, 
                    driver.preprocessor.fetch_line(stmt.line_number),
                    {
                        "Symbol \"" + stmt.ast->left->node.data  + "\" for reassignment has not yet been defined"
                    }
                )
            );
        }

        // Get the symbol type
        FORGE::DataType lhs_type = driver.symbol_table.get_value_type(stmt.ast->left->node.data);

        // Clear the temp vector for expression building
        forge_expression_items.clear();

        // Build the expression items vector
        validate_and_build_assignment(stmt.ast->left->node.data, stmt.ast->right, lhs_type, stmt.line_number);

        // Build reassignment 
        FORGE::Reassignment * reassign = new FORGE::Reassignment(
            new FORGE::Variable(stmt.ast->left->node.data, lhs_type),
            new FORGE::Expression(lhs_type, forge_expression_items)
        );

        // Add reassignment instruction to forge
        current_forge_aggregator->add_instruction(reassign);
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(If & stmt) 
    {
        switch(stmt.type)
        {
        case DEL::If::Type::IF:   
        {
            FORGE::DataType if_data_type = determine_expression_type(stmt.ast, stmt.ast, true, stmt.line_number);

            // Clear the temp vector for expression building
            forge_expression_items.clear();

            // Build the expression items vector
            validate_and_build_assignment("If Statement", stmt.ast, if_data_type, stmt.line_number);

            // Create the if function
            FORGE::If * if_statement = new FORGE::If(new FORGE::Expression(if_data_type, forge_expression_items));

            // Push it as the currrent aggregator
            aggregators.push(if_statement);

            // Set the aggregator
            current_forge_aggregator = aggregators.top();

            // Add elements
            for(auto & el : stmt.elements)
            {
                el->visit(*this);
                delete el;
            }

            // Remove if function
            aggregators.pop();

            // Reset aggregator
            current_forge_aggregator = aggregators.top();

            // Add the built statement to whatever the if statement was in
            current_forge_aggregator->add_instruction(if_statement);

            // If the statement has a trailing else of whatever, we need to folow it
            if(stmt.trail)
            {
                stmt.trail->visit(*this);
            }
            return;
        }
        case DEL::If::Type::ELIF:
        {
            FORGE::DataType elif_data_type = determine_expression_type(stmt.ast, stmt.ast, true, stmt.line_number);

            // Clear the temp vector for expression building
            forge_expression_items.clear();

            // Build the expression items vector
            validate_and_build_assignment("Else If Statement", stmt.ast, elif_data_type, stmt.line_number);

            // Create the if function
            FORGE::Elif * elif_statement = new FORGE::Elif(new FORGE::Expression(elif_data_type, forge_expression_items));

            // Push it as the currrent aggregator
            aggregators.push(elif_statement);

            // Set the aggregator
            current_forge_aggregator = aggregators.top();

            // Add elements
            for(auto & el : stmt.elements)
            {
                el->visit(*this);
                delete el;
            }

            // Remove if function
            aggregators.pop();

            // Reset aggregator
            current_forge_aggregator = aggregators.top();

            // Add the built statement to whatever the if statement was in
            current_forge_aggregator->add_instruction(elif_statement);

            // If the statement has a trailing else of whatever, we need to folow it
            if(stmt.trail)
            {
                stmt.trail->visit(*this);
            }
            return;
        }
        /*
            ELSE statements are just ELIF(1) statements, so we don't actually handle them in a special way. 
            This is also stated in the grammar file
        */
        }
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(WhileLoop  &stmt)
    {
        FORGE::DataType data_type = determine_expression_type(stmt.ast, stmt.ast, true, stmt.line_number);

        // Clear the temp vector for expression building
        forge_expression_items.clear();

        // Build the expression items vector
        validate_and_build_assignment("While Loop", stmt.ast, data_type, stmt.line_number);

        // Create the if function
        FORGE::While * while_loop = new FORGE::While(new FORGE::Expression(data_type, forge_expression_items));

        // Push it as the currrent aggregator
        aggregators.push(while_loop);

        // Set the aggregator
        current_forge_aggregator = aggregators.top();

        current_forge_continuable = while_loop;

        // Add elements
        for(auto & el : stmt.elements)
        {
            el->visit(*this);
            delete el;
        }

        // Remove if function
        aggregators.pop();

        // Reset aggregator
        current_forge_aggregator = aggregators.top();

        // Add the built statement to whatever the if statement was in
        current_forge_aggregator->add_instruction(while_loop);

        current_forge_continuable = nullptr;
    }
    
    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(ForLoop    &stmt)
    {
        FORGE::DataType data_type = determine_expression_type(stmt.condition, stmt.condition, true, stmt.line_number);

        // Initialize the loop variable before the loop
        stmt.loop_var->visit(*this);

        // Clear the temp vector for expression building
        forge_expression_items.clear();

        // Build the expression items vector
        validate_and_build_assignment("For Loop", stmt.condition, data_type, stmt.line_number);

        FORGE::For * for_loop = new FORGE::For(new FORGE::Expression(data_type, forge_expression_items));

        // Push it as the currrent aggregator
        aggregators.push(for_loop);

        // Set the aggregator
        current_forge_aggregator = aggregators.top();

        current_forge_continuable = for_loop;

        // Add elements
        for(auto & el : stmt.elements)
        {
            el->visit(*this);
            delete el;
        }

        // Add the "step" x++ etc to the loop
        stmt.step->visit(*this);

        // Remove if function
        aggregators.pop();

        // Reset aggregator
        current_forge_aggregator = aggregators.top();

        // Add the built statement to whatever the if statement was in
        current_forge_aggregator->add_instruction(for_loop);

        current_forge_continuable = nullptr;
    }
    
    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(NamedLoop  &stmt)
    {
        /*
            Create an expression that is 

            -> name = 1
        */
        Assignment * assign = new DEL::Assignment(
               false, /* Not immutable */
               new DEL::Ast(DEL::Ast::NodeType::ROOT, FORGE::DataType::UNDEFINED, "=", 
                  new DEL::Ast(DEL::Ast::NodeType::IDENTIFIER, FORGE::DataType::UNKNOWN, stmt.name , nullptr, nullptr), /* Var name */
                  new DEL::Ast(DEL::Ast::NodeType::VALUE, FORGE::DataType::STANDARD_INTEGER, "1", nullptr, nullptr)),  /* Expression AST    */
               new EncodedDataType(FORGE::DataType::STANDARD_INTEGER,    "int"   ),      /* Encoded Data type */
            stmt.line_number);        /* Line Number       */

        // Build the assignment for the var 'name = 1' for the loop
        assign->visit(*this);

        // Build the loop

        // Clear the temp vector for expression building
        forge_expression_items.clear();

        // Build the expression items vector
        validate_and_build_assignment("Named Loop", 
            new DEL::Ast(                                   // Here we create an AST that represents
                DEL::Ast::NodeType::IDENTIFIER,             // The expression :    (name)
                FORGE::DataType::UNKNOWN,                   // As we set name = 1 above so this will be the while-condition
                stmt.name, 
                nullptr, 
                nullptr), 
                    FORGE::DataType::STANDARD_INTEGER, 
                    stmt.line_number
        );

        // Create the if function
        FORGE::While * while_loop = new FORGE::While(new FORGE::Expression(FORGE::DataType::STANDARD_INTEGER, forge_expression_items));

        // Name loops are set to be breakable, so we make this while a breakable 
        current_forge_breakable = while_loop;
        current_forge_continuable = while_loop;

        // Push it as the currrent aggregator
        aggregators.push(while_loop);

        // Set the aggregator
        current_forge_aggregator = aggregators.top();

        // Add elements
        for(auto & el : stmt.elements)
        {
            el->visit(*this);
            delete el;
        }

        // Remove if function
        aggregators.pop();

        // Reset aggregator
        current_forge_aggregator = aggregators.top();

        // Add the built statement to whatever the if statement was in
        current_forge_aggregator->add_instruction(while_loop);
        
        current_forge_continuable = nullptr;
        current_forge_breakable = nullptr;
    }
    
    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(Continue   &stmt)
    {
        // This will be fine as the only way a continue statement can come in is if it is within
        // the scope of a loop, and loops implement this interface

        if(current_forge_continuable)
        {        
            current_forge_continuable->add_continue_statement();
        }
        else
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::InternalReport(
                    {
                        "DEL::Analyzer",
                        "Analyzer.cpp",
                        "accept(Continue &stmt)",
                        {
                            "A continue statement came in and the continuable pointer was not set",
                            "This could either be a grammar error, or an implementation error in the analyzer",
                            "Either way this is a developer error, not a user error"
                        }
                    }
                )
            );
        }
    }
    
    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::accept(Break      &stmt)
    {
        // This will be fine as the only way a break statement can come in is if it is within
        // the scope of a named loop

        if(current_forge_breakable)
        {        
            current_forge_breakable->add_break(stmt.name);
        }
        else
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::InternalReport(
                    {
                        "DEL::Analyzer",
                        "Analyzer.cpp",
                        "accept(Break &stmt)",
                        {
                            "A break statement came in and the breakable pointer was not set",
                            "This could either be a grammar error, or an implementation error in the analyzer",
                            "Either way this is a developer error, not a user error"
                        }
                    }
                )
            );
        }
    }

    //
    // ===============---------------------------- Analysis Methods ---------------------------===============
    //

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    void Analyzer::ensure_id_in_current_context(std::string id, int line_no, std::vector<FORGE::DataType> allowed)
    {
        // Check symbol table to see if an id exists, don't display information yet
        if(!driver.symbol_table.does_symbol_exist(id, false))
        {
            // Reports the error and true marks the program for death
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(line_no),
                    -1, 
                    driver.preprocessor.fetch_line(line_no),
                    {
                        "Unknown identifier \"" + id + "\"" 
                    }
                )
            );
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
            if(driver.symbol_table.is_existing_symbol_of_type(id, v))
            {
                is_allowed = true;
            }
        }

        // If the type isn't allowed
        if(!is_allowed)
        {
            // Reports the error and true marks the program for death
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(line_no),
                    -1, 
                    driver.preprocessor.fetch_line(line_no),
                    {
                        "Type of identifier \"" + id + "\" (" + 
                            FORGE::DataType_to_string (driver.symbol_table.get_value_type(id)) + 
                            ") not permitted in current operation", 
                    }
                )
            );
        }
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::validate_call(Call & stmt)
    {
        // Disallow recursion until we come up with a way to handle it in the ASM
        //
        if(stmt.function_name == current_front_function->name)
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::InternalReport(
                    {
                        "DEL::Analyzer",
                        "Analyzer.cpp",
                        "validate_call",
                        {
                            "Recursion is not yet supported. A recursive call was detected on line : " + 
                            std::to_string(stmt.line_number) + " of file : " + driver.current_file_from_directive
                        }
                    }
                )
            );
        }

        //  Check if the context exists
        //
        if(!driver.symbol_table.does_context_exist(stmt.function_name))
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(stmt.line_number),
                    -1, 
                    driver.preprocessor.fetch_line(stmt.line_number),
                    {
                        "Unknown function name given for call"
                    }
                )
            );
        }
      
        std::vector<FORGE::Variable> params = driver.symbol_table.get_context_parameters(stmt.function_name);

        // Ensure that the parameters are the size we expect
        if(params.size() != stmt.params.size())
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(stmt.line_number),
                    -1, 
                    driver.preprocessor.fetch_line(stmt.line_number),
                    {
                        "Mismatched number of parameters given for call to : " + stmt.function_name,
                        "Expected " + std::to_string(params.size()) + ", but given " + std::to_string(stmt.params.size())
                    }
                )
            );
        }

        //  Ensure all parameters exist, and if they do set the type (if needed)
        //
        for( auto & p : stmt.params)
        {

            // If the data type is an UNKNOWN we need to figure out its type
            // We do this with the help of the symbol table
            if(p->getType()  == FORGE::DataType::UNKNOWN)
            {
                ensure_id_in_current_context(p->getName(), stmt.line_number, {});
                switch(driver.symbol_table.get_value_type(p->getName()))
                {
                    case FORGE::DataType::STANDARD_STRING:  p->setType(FORGE::DataType::VAR_STANDARD_STRING);  break;
                    case FORGE::DataType::STANDARD_INTEGER: p->setType(FORGE::DataType::VAR_STANDARD_INTEGER); break;
                    case FORGE::DataType::STANDARD_DOUBLE:  p->setType(FORGE::DataType::VAR_STANDARD_DOUBLE);  break;
                    case FORGE::DataType::STANDARD_CHAR:    p->setType(FORGE::DataType::VAR_STANDARD_CHAR);    break;
                    default:
                        driver.code_forge.get_reporter().issue_report(
                            new FORGE::InternalReport(
                                {
                                    "DEL::Analyzer",
                                    "Analyzer.cpp",
                                    "validate_call",
                                    {
                                        "Default accessed while attempting to set a parameter variable type"
                                    }
                                }
                            )
                        );
                        break;
                }
            }
            else if(p->getType() == FORGE::DataType::REF_UNKNOWN)
            {
                ensure_id_in_current_context(p->getName(), stmt.line_number, {});
                switch(driver.symbol_table.get_value_type(p->getName()))
                {
                    case FORGE::DataType::STANDARD_STRING:  p->setType(FORGE::DataType::REF_STANDARD_STRING);  break;
                    case FORGE::DataType::STANDARD_INTEGER: p->setType(FORGE::DataType::REF_STANDARD_INTEGER); break;
                    case FORGE::DataType::STANDARD_DOUBLE:  p->setType(FORGE::DataType::REF_STANDARD_DOUBLE);  break;
                    case FORGE::DataType::STANDARD_CHAR:    p->setType(FORGE::DataType::REF_STANDARD_CHAR);    break;
                    default:
                        driver.code_forge.get_reporter().issue_report(
                            new FORGE::InternalReport(
                                {
                                    "DEL::Analyzer",
                                    "Analyzer.cpp",
                                    "validate_call",
                                    {
                                        "Default accessed while attempting to set a parameter reference type"
                                    }
                                }
                            )
                        );
                        break;
                }
            }
        }

        //  Check that the types match what we expect
        //
        for(uint16_t i = 0; i < stmt.params.size(); i++ )
        {
            /*
                We call base_equal to ensure that anything that is a *_INTEGER or *_DOUBLE etc has a matching *_DOUBLE etc
                this way a VAR_STANDARD_INTEGER will be matched as the same base type as a STANDARD_INTEGER or REF_STANDARD_INTEGER
            */
            if(!FORGE::DataType_base_equal(stmt.params[i]->getType(), params[i].getType()))
            {
                driver.code_forge.get_reporter().issue_report(
                    new FORGE::SemanticReport(
                        FORGE::Report::Level::ERROR,
                        driver.current_file_from_directive,
                        driver.preprocessor.fetch_user_line_number(stmt.line_number),
                        -1, 
                        driver.preprocessor.fetch_line(stmt.line_number),
                        {
                            "Given parameter \"" + stmt.params[i]->getName() + "\" doesn't match expected data type for call to : " + stmt.function_name,
                            "Received type  : " + FORGE::DataType_to_string(stmt.params[i]->getType()),
                            "Expected type  : " + FORGE::DataType_to_string(params[i].getType())
                        }
                    )
                );
            }
        }   
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    FORGE::DataType Analyzer::get_id_type(std::string id, int line_number)
    {
        if(!driver.symbol_table.does_symbol_exist(id))
        {
            driver.code_forge.get_reporter().issue_report(
                new FORGE::SemanticReport(
                    FORGE::Report::Level::ERROR,
                    driver.current_file_from_directive,
                    driver.preprocessor.fetch_user_line_number(line_number),
                    -1, 
                    driver.preprocessor.fetch_line(line_number),
                    {
                        "Symbol \"" + id  + "\" used in expression does not exist"
                    }
                )
            );
        }

        return driver.symbol_table.get_value_type(id);
    }

    // ----------------------------------------------------------
    //
    // ----------------------------------------------------------

    FORGE::DataType Analyzer::determine_expression_type(Ast * ast, Ast * traverse, bool left, int line_no)
    {
        if(ast->node.node_type == Ast::NodeType::VALUE)
        {
            return ast->node.data_type;
        }
        else if (ast->node.node_type == Ast::NodeType::IDENTIFIER)
        {
            return get_id_type(ast->node.data, line_no);
        }
        else if (ast->node.node_type == Ast::NodeType::CALL)
        {
            Call * call = static_cast<Call*>(ast);

            validate_call(*call);

            return driver.symbol_table.get_return_type_of_context(call->function_name);
        }
        if(left)
        {
            // This should never happen, but we handle it just in case
            if(ast->left == nullptr)
            {
                return determine_expression_type(traverse, traverse, false, line_no);
            }

            // Go down left side - we only need to traverse one side
            return determine_expression_type(ast->right, traverse, true, line_no);
        }
        else
        {
            // This REALLY should't happen.
            if(ast->right == nullptr)
            {
                driver.code_forge.get_reporter().issue_report(
                    new FORGE::InternalReport(
                        {
                            "DEL::Analyzer",
                            "Analyzer.cpp",
                            "determine_expression_type",
                            {
                                "Developer error : Failed to determine expression type"
                            }
                        }
                    )
                );
            }

            return determine_expression_type(ast->right, traverse, false, line_no);
        }
    }

    // -----------------------------------------------------
    //
    // -----------------------------------------------------

    void Analyzer::validate_and_build_assignment(std::string var_name, Ast * ast, FORGE::DataType type, int line_number)
    {
        switch(ast->node.node_type)
        {
            //
            //      IDENTIFIER
            //
            case Ast::NodeType::IDENTIFIER:  
            {

                // Ensure the symbol to be reassigned has already been defined
                if(!driver.symbol_table.does_symbol_exist(ast->node.data))
                {
                    driver.code_forge.get_reporter().issue_report(
                        new FORGE::SemanticReport(
                            FORGE::Report::Level::ERROR,
                            driver.current_file_from_directive,
                            driver.preprocessor.fetch_user_line_number(line_number),
                            -1, 
                            driver.preprocessor.fetch_line(line_number),
                            {
                                "Symbol \"" + ast->node.data  + "\" used in expression does not exist"
                            }
                        )
                    );
                }

                // Ensure variable's type is compliant
                if(driver.symbol_table.get_value_type(ast->node.data) != type)
                {
                    driver.code_forge.get_reporter().issue_report(
                        new FORGE::SemanticReport(
                            FORGE::Report::Level::ERROR,
                            driver.current_file_from_directive,
                            driver.preprocessor.fetch_user_line_number(line_number),
                            -1, 
                            driver.preprocessor.fetch_line(line_number),
                            {
                                "Type of \"" + ast->node.data + "\" is \"" + FORGE::DataType_to_string(ast->node.data_type) + 
                                "\", which is incompatible with type of \"" + var_name + 
                                "\" which is type \"" + FORGE::DataType_to_string(type) + "\"\n"
                            }
                        )
                    );
                }

                // Indicate that value is a variable and hand the data
                forge_expression_items.push_back({FORGE::Expression::Instruction::VARIABLE, ast->node.data});
                return;
            }
            //
            //      VALUE
            //
            case Ast::NodeType::VALUE:  
            {
                // Ensure that the types align. int-int | double-double  | string-string
                // That way we know how to handle things in the rear end
                if(ast->node.data_type != type)
                {
                    driver.code_forge.get_reporter().issue_report(
                        new FORGE::SemanticReport(
                            FORGE::Report::Level::ERROR,
                            driver.current_file_from_directive,
                            driver.preprocessor.fetch_user_line_number(line_number),
                            -1, 
                            driver.preprocessor.fetch_line(line_number),
                            {
                                "Type of \"" + ast->node.data + "\" is \"" + FORGE::DataType_to_string(ast->node.data_type) + 
                                "\", which is incompatible with type of \"" + var_name + 
                                "\" which is type \"" + FORGE::DataType_to_string(type) + "\"\n"
                            }
                        )
                    );
                }

                // Indicate in the expression that we have a value, and store the data
                forge_expression_items.push_back({FORGE::Expression::Instruction::VALUE, ast->node.data});
                return;
            }
            //
            //      EXPR CALL
            //
            case Ast::NodeType::CALL:  
            {   
                // The AST pointer should be able to be a call now, because, its awesome
                Call * call = static_cast<Call*>(ast);

                // Validate the call
                validate_call(*call);

                forge_expression_items.push_back({FORGE::Expression::Instruction::CALL, ast->node.data, call->params});
                return;
            }
            case Ast::NodeType::ROOT:  
            {
                driver.code_forge.get_reporter().issue_report(
                    new FORGE::InternalReport(
                        {
                            "DEL::Analyzer",
                            "Analyzer.cpp",
                            "validate_and_build_assignment",
                            {
                                "A ROOT node slipped into function. The setup of Analyzer should not have allowed this"
                            }
                        }
                    )
                );
                return;
            }
            case Ast::NodeType::ADD:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::ADD, ""});
                return;
            }
            case Ast::NodeType::SUB:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::SUB, ""});
                return;
            }
            case Ast::NodeType::LTE:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::LTE, ""});
                return;
            }
            case Ast::NodeType::GTE:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::GTE, ""});
                return;
            }
            case Ast::NodeType::GT:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::GT, ""});
                return;
            }
            case Ast::NodeType::LT:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::LT, ""});
                return;
            }
            case Ast::NodeType::EQ:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::EQ, ""});
                return;
            }
            case Ast::NodeType::NE:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::NE, ""});
                return;
            }
            case Ast::NodeType::MUL:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::MUL, ""});
                return;
            }
            case Ast::NodeType::DIV:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::DIV, ""});
                return;
            }
            case Ast::NodeType::POW:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::POW, ""});
                return;
            }
            case Ast::NodeType::MOD:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::MOD, ""});
                return;
            }
            case Ast::NodeType::LSH:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::LSH, ""});
                return;
            }
            case Ast::NodeType::RSH:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::RSH, ""});
                return;
            }
            case Ast::NodeType::BW_XOR:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::BW_XOR, ""});
                return;
            }
            case Ast::NodeType::BW_OR:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::BW_OR, ""});
                return;
            } 
            case Ast::NodeType::BW_AND:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::BW_AND, ""});
                return;
            }
            case Ast::NodeType::OR:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::OR, ""});
                return;
            }
            case Ast::NodeType::AND:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::AND, ""});
                return;
            }
            case Ast::NodeType::BW_NOT:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::BW_NOT, ""});
                return;
            }
            case Ast::NodeType::NEGATE:  
            {
                validate_and_build_assignment(var_name, ast->left, type, line_number); validate_and_build_assignment(var_name, ast->right, type, line_number);
                forge_expression_items.push_back({FORGE::Expression::Instruction::NEGATE, ""});
                return;
            }
            default:
            {
                driver.code_forge.get_reporter().issue_report(
                    new FORGE::InternalReport(
                        {
                            "DEL::Analyzer",
                            "Analyzer.cpp",
                            "validate_and_build_assignment",
                            {
                                "Default was accessed while walking the tree. This means a new AST node type was most likely added and not handled."
                            }
                        }
                    )
                );
                break;
            }
        }
    }
}
