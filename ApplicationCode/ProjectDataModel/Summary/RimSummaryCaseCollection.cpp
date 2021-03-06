/////////////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2017-     Statoil ASA
//
//  ResInsight is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  ResInsight is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.
//
//  See the GNU General Public License at <http://www.gnu.org/licenses/gpl.html>
//  for more details.
//
/////////////////////////////////////////////////////////////////////////////////

#include "RimSummaryCaseCollection.h"

#include "RiaApplication.h"
#include "RiaFieldHandleTools.h"

#include "RicfCommandObject.h"

#include "RimDerivedEnsembleCaseCollection.h"
#include "RimEnsembleCurveSet.h"
#include "RimGridSummaryCase.h"
#include "RimProject.h"
#include "RimSummaryCase.h"

#include "RifReaderEclipseRft.h"
#include "RifReaderEnsembleStatisticsRft.h"
#include "RifSummaryReaderInterface.h"

#include "cafPdmFieldIOScriptability.h"

#include <QFileDialog>
#include <QMessageBox>

#include <algorithm>
#include <cmath>

CAF_PDM_SOURCE_INIT( RimSummaryCaseCollection, "SummaryCaseSubCollection" );

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
double EnsembleParameter::stdDeviation() const
{
    double N = static_cast<double>( values.size() );
    if ( N > 1 && isNumeric() )
    {
        double sumValues        = 0.0;
        double sumValuesSquared = 0.0;
        for ( const QVariant& variant : values )
        {
            double value = variant.toDouble();
            sumValues += value;
            sumValuesSquared += value * value;
        }

        return std::sqrt( ( N * sumValuesSquared - sumValues * sumValues ) / ( N * ( N - 1.0 ) ) );
    }
    return 0.0;
}

//--------------------------------------------------------------------------------------------------
/// Standard deviation normalized by max absolute value of min/max values.
/// Produces values between 0.0 and sqrt(2.0).
//--------------------------------------------------------------------------------------------------
double EnsembleParameter::normalizedStdDeviation() const
{
    const double eps = 1.0e-4;

    double maxAbs = std::max( std::fabs( maxValue ), std::fabs( minValue ) );
    if ( maxAbs < eps )
    {
        return 0.0;
    }

    double normalisedStdDev = stdDeviation() / maxAbs;
    if ( normalisedStdDev < eps )
    {
        return 0.0;
    }
    return normalisedStdDev;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void EnsembleParameter::sortByBinnedVariation( std::vector<NameParameterPair>& parameterVector )
{
    double minStdDev = std::numeric_limits<double>::infinity();
    double maxStdDev = 0.0;
    for ( const auto& paramPair : parameterVector )
    {
        minStdDev = std::min( minStdDev, paramPair.second.normalizedStdDeviation() );
        maxStdDev = std::max( maxStdDev, paramPair.second.normalizedStdDeviation() );
    }
    if ( ( maxStdDev - minStdDev ) < 1.0e-8 )
    {
        return;
    }

    double delta = ( maxStdDev - minStdDev ) / NR_OF_VARIATION_BINS;

    std::vector<double> bins;
    for ( int i = 0; i < NR_OF_VARIATION_BINS - 1; ++i )
    {
        bins.push_back( minStdDev + ( i + 1 ) * delta );
    }

    for ( NameParameterPair& nameParamPair : parameterVector )
    {
        int binNumber = 0;
        for ( double bin : bins )
        {
            if ( nameParamPair.second.normalizedStdDeviation() >= bin )
            {
                binNumber++;
            }
        }
        nameParamPair.second.variationBin = binNumber;
    }

    // Sort by variation bin (highest first) but keep name as sorting parameter when parameters have the same variation
    // index
    std::stable_sort( parameterVector.begin(),
                      parameterVector.end(),
                      [&bins]( const NameParameterPair& lhs, const NameParameterPair& rhs ) {
                          return lhs.second.variationBin > rhs.second.variationBin;
                      } );
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
QString EnsembleParameter::uiName( const NameParameterPair& paramPair )
{
    QString stem = paramPair.first;
    QString variationString;
    if ( paramPair.second.isNumeric() )
    {
        switch ( paramPair.second.variationBin )
        {
            case LOW_VARIATION:
                variationString = QString( " (Low variation)" );
            case MEDIUM_VARIATION:
                break;
            case HIGH_VARIATION:
                variationString = QString( " (High variation)" );
                break;
        }
    }
    return QString( "%1%2" ).arg( stem ).arg( variationString );
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
RimSummaryCaseCollection::RimSummaryCaseCollection()
{
    CAF_PDM_InitScriptableObject( "Summary Case Group", ":/SummaryGroup16x16.png", "", "" );

    CAF_PDM_InitFieldNoDefault( &m_cases, "SummaryCases", "", "", "", "" );
    m_cases.uiCapability()->setUiHidden( true );

    CAF_PDM_InitScriptableFieldWithIO( &m_name, "SummaryCollectionName", QString( "Group" ), "Name", "", "", "" );

    CAF_PDM_InitScriptableFieldWithIONoDefault( &m_nameAndItemCount, "NameCount", "Name", "", "", "" );
    m_nameAndItemCount.registerGetMethod( this, &RimSummaryCaseCollection::nameAndItemCount );
    RiaFieldhandleTools::disableWriteAndSetFieldHidden( &m_nameAndItemCount );

    CAF_PDM_InitScriptableFieldWithIO( &m_isEnsemble, "IsEnsemble", false, "Is Ensemble", "", "", "" );
    m_isEnsemble.uiCapability()->setUiHidden( true );

    CAF_PDM_InitScriptableFieldWithIO( &m_ensembleId, "Id", -1, "Ensemble ID", "", "", "" );
    m_ensembleId.registerKeywordAlias( "EnsembleId" );
    m_ensembleId.uiCapability()->setUiReadOnly( true );
    m_ensembleId.capability<caf::PdmFieldScriptability>()->setIOWriteable( false );

    m_statisticsEclipseRftReader = new RifReaderEnsembleStatisticsRft( this );

    m_commonAddressCount = 0;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
RimSummaryCaseCollection::~RimSummaryCaseCollection()
{
    m_cases.deleteAllChildObjectsAsync();
    updateReferringCurveSets();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::removeCase( RimSummaryCase* summaryCase )
{
    size_t caseCountBeforeRemove = m_cases.size();
    m_cases.removeChildObject( summaryCase );
    updateReferringCurveSets();

    if ( m_isEnsemble && m_cases.size() != caseCountBeforeRemove )
    {
        if ( dynamic_cast<RimDerivedSummaryCase*>( summaryCase ) == nullptr )
            calculateEnsembleParametersIntersectionHash();
    }
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::addCase( RimSummaryCase* summaryCase, bool updateCurveSets )
{
    m_cases.push_back( summaryCase );

    // Update derived ensemble cases (if any)
    std::vector<RimDerivedEnsembleCaseCollection*> referringObjects;
    objectsWithReferringPtrFieldsOfType( referringObjects );
    for ( auto derEnsemble : referringObjects )
    {
        if ( !derEnsemble ) continue;

        derEnsemble->updateDerivedEnsembleCases();
        if ( updateCurveSets ) derEnsemble->updateReferringCurveSets();
    }

    if ( m_isEnsemble )
    {
        validateEnsembleCases( {summaryCase} );
        calculateEnsembleParametersIntersectionHash();
    }

    if ( updateCurveSets ) updateReferringCurveSets();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
std::vector<RimSummaryCase*> RimSummaryCaseCollection::allSummaryCases() const
{
    return m_cases.childObjects();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::setName( const QString& name )
{
    m_name = name;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
QString RimSummaryCaseCollection::name() const
{
    return m_name;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
bool RimSummaryCaseCollection::isEnsemble() const
{
    return m_isEnsemble();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::setAsEnsemble( bool isEnsemble )
{
    if ( isEnsemble != m_isEnsemble )
    {
        m_isEnsemble = isEnsemble;
        updateIcon();

        if ( m_isEnsemble && dynamic_cast<RimDerivedEnsembleCaseCollection*>( this ) == nullptr )
        {
            validateEnsembleCases( allSummaryCases() );
            calculateEnsembleParametersIntersectionHash();
        }
    }
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
std::set<RifEclipseSummaryAddress> RimSummaryCaseCollection::ensembleSummaryAddresses() const
{
    std::set<RifEclipseSummaryAddress> addresses;
    size_t                             maxAddrCount = 0;
    int                                maxAddrIndex = -1;

    for ( int i = 0; i < (int)m_cases.size(); i++ )
    {
        RimSummaryCase* currCase = m_cases[i];
        if ( !currCase ) continue;

        RifSummaryReaderInterface* reader = currCase->summaryReader();
        if ( !reader ) continue;

        size_t addrCount = reader->allResultAddresses().size();
        if ( addrCount > maxAddrCount )
        {
            maxAddrCount = addrCount;
            maxAddrIndex = (int)i;
        }
    }

    if ( maxAddrIndex >= 0 && m_cases[maxAddrIndex]->summaryReader() )
    {
        const std::set<RifEclipseSummaryAddress>& addrs = m_cases[maxAddrIndex]->summaryReader()->allResultAddresses();
        addresses.insert( addrs.begin(), addrs.end() );
    }
    return addresses;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
std::set<QString> RimSummaryCaseCollection::wellsWithRftData() const
{
    std::set<QString> allWellNames;
    for ( RimSummaryCase* summaryCase : m_cases )
    {
        RifReaderRftInterface* reader = summaryCase->rftReader();
        if ( reader )
        {
            std::set<QString> wellNames = reader->wellNames();
            allWellNames.insert( wellNames.begin(), wellNames.end() );
        }
    }
    return allWellNames;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
std::set<QDateTime> RimSummaryCaseCollection::rftTimeStepsForWell( const QString& wellName ) const
{
    std::set<QDateTime> allTimeSteps;
    for ( RimSummaryCase* summaryCase : m_cases )
    {
        RifReaderRftInterface* reader = summaryCase->rftReader();
        if ( reader )
        {
            std::set<QDateTime> timeStep = reader->availableTimeSteps( wellName );
            allTimeSteps.insert( timeStep.begin(), timeStep.end() );
        }
    }
    return allTimeSteps;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
RifReaderRftInterface* RimSummaryCaseCollection::rftStatisticsReader()
{
    return m_statisticsEclipseRftReader.p();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
EnsembleParameter RimSummaryCaseCollection::ensembleParameter( const QString& paramName ) const
{
    if ( !isEnsemble() || paramName.isEmpty() ) return EnsembleParameter();

    EnsembleParameter eParam;
    eParam.name = paramName;

    size_t numericValuesCount = 0;
    size_t textValuesCount    = 0;

    // Prepare case realization params, and check types
    for ( const auto& rimCase : allSummaryCases() )
    {
        auto crp = rimCase->caseRealizationParameters();
        if ( !crp ) continue;

        auto value = crp->parameterValue( paramName );
        if ( !value.isValid() ) continue;

        if ( value.isNumeric() )
        {
            double numVal = value.numericValue();
            eParam.values.push_back( QVariant( numVal ) );
            if ( numVal < eParam.minValue ) eParam.minValue = numVal;
            if ( numVal > eParam.maxValue ) eParam.maxValue = numVal;
            numericValuesCount++;
        }
        else if ( value.isText() )
        {
            eParam.values.push_back( QVariant( value.textValue() ) );
            textValuesCount++;
        }
    }

    if ( numericValuesCount && !textValuesCount )
    {
        eParam.type = EnsembleParameter::TYPE_NUMERIC;
    }
    else if ( textValuesCount && !numericValuesCount )
    {
        eParam.type = EnsembleParameter::TYPE_TEXT;
    }
    if ( numericValuesCount && textValuesCount )
    {
        // A mix of types have been added to parameter values
        if ( numericValuesCount > textValuesCount )
        {
            // Use numeric type
            for ( auto& val : eParam.values )
            {
                if ( val.type() == QVariant::String )
                {
                    val.setValue( std::numeric_limits<double>::infinity() );
                }
            }
            eParam.type = EnsembleParameter::TYPE_NUMERIC;
        }
        else
        {
            // Use text type
            for ( auto& val : eParam.values )
            {
                if ( val.type() == QVariant::Double )
                {
                    val.setValue( QString::number( val.value<double>() ) );
                }
            }
            eParam.type     = EnsembleParameter::TYPE_TEXT;
            eParam.minValue = std::numeric_limits<double>::infinity();
            eParam.maxValue = -std::numeric_limits<double>::infinity();
        }
    }

    if ( eParam.isText() )
    {
        // Remove duplicate texts
        std::set<QString> valueSet;
        for ( const auto& val : eParam.values )
        {
            valueSet.insert( val.toString() );
        }
        eParam.values.clear();
        for ( const auto& val : valueSet )
        {
            eParam.values.push_back( QVariant( val ) );
        }
    }

    return eParam;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::calculateEnsembleParametersIntersectionHash()
{
    clearEnsembleParametersHashes();

    // Find ensemble parameters intersection
    std::set<QString> paramNames;
    auto              sumCases = allSummaryCases();

    for ( size_t i = 0; i < sumCases.size(); i++ )
    {
        auto crp = sumCases[i]->caseRealizationParameters();
        if ( !crp ) continue;

        auto caseParamNames = crp->parameterNames();

        if ( i == 0 )
            paramNames = caseParamNames;
        else
        {
            std::set<QString> newIntersection;
            std::set_intersection( paramNames.begin(),
                                   paramNames.end(),
                                   caseParamNames.begin(),
                                   caseParamNames.end(),
                                   std::inserter( newIntersection, newIntersection.end() ) );

            if ( paramNames.size() != newIntersection.size() ) paramNames = newIntersection;
        }
    }

    for ( auto sumCase : sumCases )
    {
        auto crp = sumCase->caseRealizationParameters();
        if ( crp ) crp->calculateParametersHash( paramNames );
    }

    // Find common addess count
    for ( const auto sumCase : sumCases )
    {
        const auto reader = sumCase->summaryReader();
        if ( !reader ) continue;
        auto currAddrCount = reader->allResultAddresses().size();

        if ( m_commonAddressCount == 0 )
        {
            m_commonAddressCount = currAddrCount;
        }
        else
        {
            if ( currAddrCount != m_commonAddressCount )
            {
                m_commonAddressCount = 0;
                break;
            }
        }
    }
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::clearEnsembleParametersHashes()
{
    for ( auto sumCase : allSummaryCases() )
    {
        auto crp = sumCase->caseRealizationParameters();
        if ( crp ) crp->clearParametersHash();
    }
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::loadDataAndUpdate()
{
    onLoadDataAndUpdate();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
bool RimSummaryCaseCollection::validateEnsembleCases( const std::vector<RimSummaryCase*> cases )
{
    // Validate ensemble parameters
    try
    {
        QString                errors;
        std::hash<std::string> paramsHasher;
        size_t                 paramsHash = 0;

        for ( RimSummaryCase* rimCase : cases )
        {
            if ( rimCase->caseRealizationParameters() == nullptr ||
                 rimCase->caseRealizationParameters()->parameters().empty() )
            {
                errors.append( QString( "The case %1 has no ensemble parameters\n" )
                                   .arg( QFileInfo( rimCase->summaryHeaderFilename() ).fileName() ) );
            }
            else
            {
                QString paramNames;
                for ( std::pair<QString, RigCaseRealizationParameters::Value> paramPair :
                      rimCase->caseRealizationParameters()->parameters() )
                {
                    paramNames.append( paramPair.first );
                }

                size_t currHash = paramsHasher( paramNames.toStdString() );
                if ( paramsHash == 0 )
                {
                    paramsHash = currHash;
                }
                else if ( paramsHash != currHash )
                {
                    throw QString( "Ensemble parameters differ between cases" );
                }
            }
        }

        if ( !errors.isEmpty() )
        {
            QString textToDisplay = errors.left( 500 );

            textToDisplay.prepend( "Missing ensemble parameters\n\n" );

            textToDisplay.append( "\n" );
            textToDisplay.append( "No parameters file (parameters.txt or runspecification.xml) was found in \n" );
            textToDisplay.append( "the searched folders. ResInsight searches the home folder of the summary \n" );
            textToDisplay.append( "case file and the three folder levels above that.\n" );

            throw textToDisplay;
        }
        return true;
    }
    catch ( QString errorMessage )
    {
        QMessageBox mbox;
        mbox.setIcon( QMessageBox::Icon::Warning );
        mbox.setText( errorMessage );
        mbox.exec();
        return false;
    }
}

//--------------------------------------------------------------------------------------------------
/// Sorting operator for sets and maps. Sorts by name.
//--------------------------------------------------------------------------------------------------
bool RimSummaryCaseCollection::operator<( const RimSummaryCaseCollection& rhs ) const
{
    return name() < rhs.name();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
RiaEclipseUnitTools::UnitSystem RimSummaryCaseCollection::unitSystem() const
{
    if ( m_cases.empty() )
    {
        return RiaEclipseUnitTools::UNITS_UNKNOWN;
    }
    return m_cases[0]->unitsSystem();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
caf::PdmFieldHandle* RimSummaryCaseCollection::userDescriptionField()
{
    return &m_name;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::onLoadDataAndUpdate()
{
    if ( m_isEnsemble ) calculateEnsembleParametersIntersectionHash();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::updateReferringCurveSets()
{
    // Update curve set referring to this group
    std::vector<RimEnsembleCurveSet*> referringObjects;
    objectsWithReferringPtrFieldsOfType( referringObjects );

    for ( auto curveSet : referringObjects )
    {
        bool updateParentPlot = true;
        if ( curveSet ) curveSet->loadDataAndUpdate( updateParentPlot );
    }
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
QString RimSummaryCaseCollection::nameAndItemCount() const
{
    size_t itemCount = m_cases.size();
    if ( itemCount > 20 )
    {
        return QString( "%1 (%2)" ).arg( m_name() ).arg( itemCount );
    }

    return m_name();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::updateIcon()
{
    if ( m_isEnsemble )
        setUiIconFromResourceString( ":/SummaryEnsemble16x16.png" );
    else
        setUiIconFromResourceString( ":/SummaryGroup16x16.png" );
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::initAfterRead()
{
    if ( m_ensembleId() == -1 )
    {
        RimProject* project = RiaApplication::instance()->project();
        project->assignIdToEnsemble( this );
    }

    updateIcon();
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::fieldChangedByUi( const caf::PdmFieldHandle* changedField,
                                                 const QVariant&            oldValue,
                                                 const QVariant&            newValue )
{
    if ( changedField == &m_isEnsemble )
    {
        updateIcon();
    }
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::defineUiOrdering( QString uiConfigName, caf::PdmUiOrdering& uiOrdering )
{
    uiOrdering.add( &m_name );
    if ( m_isEnsemble() )
    {
        uiOrdering.add( &m_ensembleId );
    }
    uiOrdering.skipRemainingFields( true );
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::setNameAsReadOnly()
{
    m_name.uiCapability()->setUiReadOnly( true );
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
void RimSummaryCaseCollection::setEnsembleId( int ensembleId )
{
    m_ensembleId = ensembleId;
}

//--------------------------------------------------------------------------------------------------
///
//--------------------------------------------------------------------------------------------------
int RimSummaryCaseCollection::ensembleId() const
{
    return m_ensembleId();
}
